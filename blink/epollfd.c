/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2026 OmniKit                                                     │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "blink/epollfd.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blink/assert.h"
#include "blink/atomic.h"
#include "blink/endian.h"
#include "blink/errno.h"
#include "blink/fds.h"
#include "blink/linux.h"
#include "blink/log.h"
#include "blink/syscall.h"
#include "blink/thread.h"
#include "blink/timespec.h"
#include "blink/tunables.h"
#include "blink/vfs.h"

struct BlinkEpollWatch {
  int fd;
  u32 events;
  u32 last_ready;
  u64 data;
};

struct BlinkEpollState {
  pthread_mutex_t_ lock;
  _Atomic(u32) refcount;
  int oflags;
  struct BlinkEpollWatch *watches;
  size_t count;
  size_t capacity;
};

static bool BlinkDebugEpollEnabled(void) {
  static int enabled = -1;
  if (enabled == -1) {
    const char *value = getenv("OMNIKIT_DEBUG_EPOLL");
    enabled = value && *value ? 1 : 0;
  }
  return enabled;
}

static struct BlinkEpollState *BlinkEpollAcquire(struct BlinkEpollState *state) {
  int rc;
  rc = atomic_fetch_add(&state->refcount, 1);
  unassert(rc > 0);
  return state;
}

static int BlinkEpollRelease(struct BlinkEpollState *state) {
  int rc;
  if (state == NULL) {
    return 0;
  }
  rc = atomic_fetch_sub(&state->refcount, 1);
  if (rc != 1) {
    return 0;
  }
  unassert(!pthread_mutex_destroy(&state->lock));
  free(state->watches);
  free(state);
  return 0;
}

static int BlinkEpollFreeinfo(void *data) {
  return BlinkEpollRelease((struct BlinkEpollState *)data);
}

static int BlinkEpollFstat(struct VfsInfo *info, struct stat *st) {
  if (st == NULL) {
    return efault();
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFIFO | 0600;
  st->st_nlink = 1;
  st->st_ino = info->ino;
  st->st_dev = info->dev;
  return 0;
}

static int BlinkEpollFcntl(struct VfsInfo *info, int cmd, va_list args) {
  struct BlinkEpollState *state;
  int oflags;
  state = (struct BlinkEpollState *)info->data;
  switch (cmd) {
    case F_SETFD:
      return 0;
    case F_SETFL:
      oflags = va_arg(args, int);
      LOCK(&state->lock);
      state->oflags &= ~SETFL_FLAGS;
      state->oflags |= oflags & SETFL_FLAGS;
      UNLOCK(&state->lock);
      return 0;
    default:
      return einval();
  }
}

static int BlinkEpollDupInfo(struct VfsInfo *info, struct VfsInfo **newinfo) {
  struct BlinkEpollState *state;
  if (newinfo == NULL) {
    return efault();
  }
  *newinfo = NULL;
  if (VfsCreateInfo(newinfo) == -1) {
    return -1;
  }
  state = BlinkEpollAcquire((struct BlinkEpollState *)info->data);
  (*newinfo)->data = state;
  (*newinfo)->parent = NULL;
  (*newinfo)->ino = info->ino;
  (*newinfo)->dev = info->dev;
  (*newinfo)->mode = info->mode;
  (*newinfo)->refcount = 1;
  unassert(!VfsAcquireDevice(info->device, &(*newinfo)->device));
  return 0;
}

static int BlinkPollGuestFd(struct Machine *m, int fd, u32 events,
                            u32 *ready_events) {
  int rc;
  struct Fd *blinkfd;
  struct pollfd pfd;
  int (*poll_impl)(struct pollfd *, nfds_t, int);
  LOCK(&m->system->fds.lock);
  if ((blinkfd = GetFd(&m->system->fds, fd))) {
    unassert(blinkfd->cb);
    unassert(poll_impl = blinkfd->cb->poll);
  } else {
    poll_impl = NULL;
  }
  UNLOCK(&m->system->fds.lock);
  if (poll_impl == NULL) {
    return ebadf();
  }
  pfd.fd = fd;
  pfd.events = 0;
  pfd.revents = 0;
  if (events & (EPOLLIN_LINUX | EPOLLRDNORM_LINUX | EPOLLRDBAND_LINUX |
                EPOLLRDHUP_LINUX)) {
    pfd.events |= POLLIN;
  }
  if (events & (EPOLLOUT_LINUX | EPOLLWRNORM_LINUX | EPOLLWRBAND_LINUX)) {
    pfd.events |= POLLOUT;
  }
  if (events & EPOLLPRI_LINUX) {
    pfd.events |= POLLPRI;
  }
  rc = poll_impl(&pfd, 1, 0);
  if (rc == -1) {
    return -1;
  }
  *ready_events = 0;
  if (pfd.revents & POLLIN) *ready_events |= EPOLLIN_LINUX;
  if (pfd.revents & POLLOUT) *ready_events |= EPOLLOUT_LINUX;
  if (pfd.revents & POLLPRI) *ready_events |= EPOLLPRI_LINUX;
  if (pfd.revents & POLLERR) *ready_events |= EPOLLERR_LINUX;
  if (pfd.revents & POLLHUP) *ready_events |= EPOLLHUP_LINUX;
  if (BlinkDebugEpollEnabled() && pfd.events) {
    fprintf(stderr, "[epoll] poll fd=%d events=%#x revents=%#x ready=%#x\n",
            fd, pfd.events, pfd.revents, *ready_events);
  }
  return rc;
}

static int BlinkEpollCollect(struct Machine *m, struct BlinkEpollState *state,
                             struct epoll_event_linux *events, int maxevents) {
  int rc;
  int count;
  size_t i;
  u32 ready;
  u32 reported;
  u32 interest;
  count = 0;
  LOCK(&state->lock);
  for (i = 0; i < state->count && count < maxevents; ++i) {
    rc = BlinkPollGuestFd(m, state->watches[i].fd, state->watches[i].events,
                          &ready);
    if (rc == -1) {
      if (errno == EBADF) {
        ready = EPOLLERR_LINUX | EPOLLHUP_LINUX;
      } else {
        UNLOCK(&state->lock);
        return -1;
      }
    }
    interest = state->watches[i].events | EPOLLERR_LINUX | EPOLLHUP_LINUX |
               EPOLLRDHUP_LINUX;
    if (state->watches[i].events & EPOLLET_LINUX) {
      reported = (ready & interest) & ~state->watches[i].last_ready;
      state->watches[i].last_ready = ready & interest;
    } else {
      reported = ready & interest;
    }
    if (!reported) {
      continue;
    }
    if (BlinkDebugEpollEnabled()) {
      fprintf(stderr,
              "[epoll] ready epfd-watch fd=%d interest=%#x ready=%#x report=%#x data=%#llx\n",
              state->watches[i].fd, state->watches[i].events, ready, reported,
              (unsigned long long)state->watches[i].data);
    }
    Write32(events[count].events, reported);
    Write64(events[count].data, state->watches[i].data);
    ++count;
    if (state->watches[i].events & EPOLLONESHOT_LINUX) {
      state->watches[i].events = 0;
      state->watches[i].last_ready = 0;
    }
  }
  UNLOCK(&state->lock);
  return count;
}

static int BlinkEpollPoll(struct VfsInfo **infos, struct pollfd *fds,
                          nfds_t nfds, int timeout) {
  if (infos == NULL || fds == NULL || nfds != 1 || timeout != 0) {
    return einval();
  }
  fds[0].revents = 0;
  return 0;
}

static struct VfsSystem g_epollsystem = {
    .name = "epoll",
    .nodev = true,
    .ops =
        {
            .Freeinfo = BlinkEpollFreeinfo,
            .Fstat = BlinkEpollFstat,
            .Fcntl = BlinkEpollFcntl,
            .Dup = BlinkEpollDupInfo,
            .Poll = BlinkEpollPoll,
        },
};

static struct VfsDevice g_epolldevice = {
    .mounts = NULL,
    .ops = &g_epollsystem.ops,
    .data = NULL,
    .dev = -1u,
    .refcount = 1u,
};

static int BlinkGetEpollState(int epfd, struct BlinkEpollState **out) {
  struct VfsInfo *info;
  struct BlinkEpollState *state;
  if (out == NULL) {
    return efault();
  }
  *out = NULL;
  if (VfsGetFd(epfd, &info) == -1) {
    return -1;
  }
  if (info->device != &g_epolldevice) {
    unassert(!VfsFreeInfo(info));
    return einval();
  }
  state = BlinkEpollAcquire((struct BlinkEpollState *)info->data);
  unassert(!VfsFreeInfo(info));
  *out = state;
  return 0;
}

static struct BlinkEpollWatch *BlinkFindEpollWatch(struct BlinkEpollState *state,
                                                   int fd) {
  size_t i;
  for (i = 0; i < state->count; ++i) {
    if (state->watches[i].fd == fd) {
      return state->watches + i;
    }
  }
  return NULL;
}

int BlinkEpollCreate(int flags) {
  int fd;
  struct VfsInfo *info;
  struct BlinkEpollState *state;
  if (flags & ~EPOLL_CLOEXEC_LINUX) {
    return einval();
  }
  if (!(state = calloc(1, sizeof(*state)))) {
    return enomem();
  }
  state->refcount = 1;
  state->oflags = O_RDONLY;
  unassert(!pthread_mutex_init(&state->lock, 0));
  if (VfsCreateInfo(&info) == -1) {
    BlinkEpollRelease(state);
    return -1;
  }
  info->data = state;
  info->parent = NULL;
  info->ino = (u64)(uintptr_t)state;
  info->dev = -1u;
  info->mode = S_IFIFO | 0600;
  info->refcount = 1;
  unassert(!VfsAcquireDevice(&g_epolldevice, &info->device));
  fd = VfsAddFd(info);
  if (fd == -1) {
    unassert(!VfsFreeInfo(info));
    return -1;
  }
  if (BlinkDebugEpollEnabled()) {
    fprintf(stderr, "[epoll] create fd=%d flags=%#x\n", fd, flags);
  }
  return fd;
}

int BlinkEpollCtl(int epfd, int op, int fd, u32 events, u64 data) {
  int rc;
  struct VfsInfo *info;
  struct BlinkEpollState *state;
  struct BlinkEpollWatch *watch;
  if (BlinkGetEpollState(epfd, &state) == -1) {
    return -1;
  }
  if (fd == epfd) {
    BlinkEpollRelease(state);
    return einval();
  }
  if (VfsGetFd(fd, &info) == -1) {
    BlinkEpollRelease(state);
    return -1;
  }
  if (info->device == &g_epolldevice) {
    unassert(!VfsFreeInfo(info));
    BlinkEpollRelease(state);
    return einval();
  }
  unassert(!VfsFreeInfo(info));
  LOCK(&state->lock);
  watch = BlinkFindEpollWatch(state, fd);
  rc = 0;
  switch (op) {
    case EPOLL_CTL_ADD_LINUX:
      if (watch != NULL) {
        rc = eexist();
        break;
      }
      if (state->count == state->capacity) {
        size_t newcap;
        void *newwatches;
        newcap = state->capacity ? state->capacity * 2 : 8;
        newwatches =
            realloc(state->watches, newcap * sizeof(*state->watches));
        if (newwatches == NULL) {
          rc = enomem();
          break;
        }
        state->watches = newwatches;
        state->capacity = newcap;
      }
      watch = state->watches + state->count++;
      watch->fd = fd;
      watch->events = events;
      watch->last_ready = 0;
      watch->data = data;
      break;
    case EPOLL_CTL_MOD_LINUX:
      if (watch == NULL) {
        rc = enoent();
        break;
      }
      watch->events = events;
      watch->last_ready = 0;
      watch->data = data;
      break;
    case EPOLL_CTL_DEL_LINUX:
      if (watch == NULL) {
        rc = enoent();
        break;
      }
      memmove(watch, watch + 1,
              (state->watches + state->count - (watch + 1)) * sizeof(*watch));
      --state->count;
      break;
    default:
      rc = einval();
      break;
  }
  if (BlinkDebugEpollEnabled()) {
    fprintf(stderr, "[epoll] ctl epfd=%d op=%d fd=%d events=%#x data=%#llx rc=%d\n",
            epfd, op, fd, events, (unsigned long long)data, rc);
  }
  UNLOCK(&state->lock);
  BlinkEpollRelease(state);
  return rc;
}

int BlinkEpollWait(struct Machine *m, int epfd,
                   struct epoll_event_linux *events, int maxevents,
                   struct timespec deadline) {
  int rc;
  struct timespec now;
  struct timespec wait;
  struct timespec remain;
  struct BlinkEpollState *state;
  if (maxevents <= 0) {
    return einval();
  }
  if (BlinkGetEpollState(epfd, &state) == -1) {
    return -1;
  }
  for (;;) {
    if (CheckInterrupt(m, false)) {
      BlinkEpollRelease(state);
      return eintr();
    }
    rc = BlinkEpollCollect(m, state, events, maxevents);
    if (rc != 0) {
      BlinkEpollRelease(state);
      return rc;
    }
    now = GetTime();
    if (CompareTime(now, deadline) >= 0) {
      BlinkEpollRelease(state);
      return 0;
    }
    wait = FromMilliseconds(kPollingMs);
    remain = SubtractTime(deadline, now);
    if (CompareTime(remain, wait) < 0) {
      wait = remain;
    }
    nanosleep(&wait, 0);
  }
}
