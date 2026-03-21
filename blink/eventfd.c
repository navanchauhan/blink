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
#include "blink/eventfd.h"

#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blink/assert.h"
#include "blink/atomic.h"
#include "blink/errno.h"
#include "blink/linux.h"
#include "blink/log.h"
#include "blink/syscall.h"
#include "blink/thread.h"
#include "blink/vfs.h"

struct BlinkEventfdState {
  pthread_mutex_t_ lock;
  _Atomic(u32) refcount;
  uint64_t counter;
  int oflags;
  bool semaphore;
};

static bool BlinkDebugEventfdEnabled(void) {
  static int enabled = -1;
  if (enabled == -1) {
    const char *value = getenv("OMNIKIT_DEBUG_EVENTFD");
    enabled = value && *value ? 1 : 0;
  }
  return enabled;
}

static size_t BlinkIovSize(const struct iovec *iov, int iovcnt) {
  int i;
  size_t total = 0;
  for (i = 0; i < iovcnt; ++i) {
    total += iov[i].iov_len;
  }
  return total;
}

static void BlinkIovCopyIn(void *dst, const struct iovec *iov, int iovcnt,
                           size_t size) {
  int i;
  size_t copied = 0;
  for (i = 0; i < iovcnt && copied < size; ++i) {
    size_t part = iov[i].iov_len;
    if (part > size - copied) {
      part = size - copied;
    }
    memcpy((char *)dst + copied, iov[i].iov_base, part);
    copied += part;
  }
}

static void BlinkIovCopyOut(const struct iovec *iov, int iovcnt,
                            const void *src, size_t size) {
  int i;
  size_t copied = 0;
  for (i = 0; i < iovcnt && copied < size; ++i) {
    size_t part = iov[i].iov_len;
    if (part > size - copied) {
      part = size - copied;
    }
    memcpy(iov[i].iov_base, (const char *)src + copied, part);
    copied += part;
  }
}

static struct BlinkEventfdState *BlinkEventfdAcquire(
    struct BlinkEventfdState *state) {
  int rc;
  rc = atomic_fetch_add(&state->refcount, 1);
  unassert(rc > 0);
  return state;
}

static int BlinkEventfdRelease(struct BlinkEventfdState *state) {
  int rc;
  if (state == NULL) {
    return 0;
  }
  rc = atomic_fetch_sub(&state->refcount, 1);
  if (rc != 1) {
    return 0;
  }
  unassert(!pthread_mutex_destroy(&state->lock));
  free(state);
  return 0;
}

static int BlinkEventfdFreeinfo(void *data) {
  return BlinkEventfdRelease((struct BlinkEventfdState *)data);
}

static int BlinkEventfdFstat(struct VfsInfo *info, struct stat *st) {
  if (st == NULL) {
    return efault();
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFIFO | 0600;
  st->st_nlink = 1;
  st->st_ino = info->ino;
  st->st_dev = info->dev;
  st->st_blksize = sizeof(uint64_t);
  return 0;
}

static int BlinkEventfdFcntl(struct VfsInfo *info, int cmd, va_list args) {
  struct BlinkEventfdState *state;
  int oflags;
  state = (struct BlinkEventfdState *)info->data;
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

static ssize_t BlinkEventfdReadv(struct VfsInfo *info, const struct iovec *iov,
                                 int iovcnt) {
  uint64_t value;
  struct BlinkEventfdState *state;
  // Linux eventfd accepts any buffer of at least 8 bytes and transfers 8.
  if (BlinkIovSize(iov, iovcnt) < sizeof(value)) {
    return einval();
  }
  state = (struct BlinkEventfdState *)info->data;
  LOCK(&state->lock);
  if (!state->counter) {
    UNLOCK(&state->lock);
    return eagain();
  }
  if (state->semaphore) {
    value = 1;
    --state->counter;
  } else {
    value = state->counter;
    state->counter = 0;
  }
  UNLOCK(&state->lock);
  if (BlinkDebugEventfdEnabled()) {
    fprintf(stderr, "[eventfd] read -> %llu\n",
            (unsigned long long)value);
  }
  BlinkIovCopyOut(iov, iovcnt, &value, sizeof(value));
  return sizeof(value);
}

static ssize_t BlinkEventfdWritev(struct VfsInfo *info, const struct iovec *iov,
                                  int iovcnt) {
  uint64_t value;
  struct BlinkEventfdState *state;
  // Linux eventfd accepts any buffer of at least 8 bytes and consumes 8.
  if (BlinkIovSize(iov, iovcnt) < sizeof(value)) {
    return einval();
  }
  value = 0;
  BlinkIovCopyIn(&value, iov, iovcnt, sizeof(value));
  if (value == UINT64_MAX) {
    return einval();
  }
  state = (struct BlinkEventfdState *)info->data;
  LOCK(&state->lock);
  if (state->counter > UINT64_MAX - 1 - value) {
    UNLOCK(&state->lock);
    return eagain();
  }
  state->counter += value;
  if (BlinkDebugEventfdEnabled()) {
    fprintf(stderr, "[eventfd] write %llu counter=%llu\n",
            (unsigned long long)value, (unsigned long long)state->counter);
  }
  UNLOCK(&state->lock);
  return sizeof(value);
}

static int BlinkEventfdPoll(struct VfsInfo **infos, struct pollfd *fds,
                            nfds_t nfds, int timeout) {
  struct BlinkEventfdState *state;
  if (infos == NULL || fds == NULL || nfds != 1 || timeout != 0) {
    return einval();
  }
  state = (struct BlinkEventfdState *)infos[0]->data;
  LOCK(&state->lock);
  fds[0].revents = 0;
  if ((fds[0].events & POLLIN) && state->counter) {
    fds[0].revents |= POLLIN;
  }
  if ((fds[0].events & POLLOUT) && state->counter != UINT64_MAX - 1) {
    fds[0].revents |= POLLOUT;
  }
  if (BlinkDebugEventfdEnabled() && fds[0].events && fds[0].revents) {
    fprintf(stderr, "[eventfd] poll events=%#x revents=%#x counter=%llu\n",
            fds[0].events, fds[0].revents,
            (unsigned long long)state->counter);
  }
  UNLOCK(&state->lock);
  return fds[0].revents ? 1 : 0;
}

static int BlinkEventfdDup(struct VfsInfo *info, struct VfsInfo **newinfo) {
  struct BlinkEventfdState *state;
  if (newinfo == NULL) {
    return efault();
  }
  *newinfo = NULL;
  if (VfsCreateInfo(newinfo) == -1) {
    return -1;
  }
  state = BlinkEventfdAcquire((struct BlinkEventfdState *)info->data);
  (*newinfo)->data = state;
  (*newinfo)->parent = NULL;
  (*newinfo)->ino = info->ino;
  (*newinfo)->dev = info->dev;
  (*newinfo)->mode = info->mode;
  (*newinfo)->refcount = 1;
  unassert(!VfsAcquireDevice(info->device, &(*newinfo)->device));
  return 0;
}

static struct VfsSystem g_eventfdsystem = {
    .name = "eventfd",
    .nodev = true,
    .ops =
        {
            .Freeinfo = BlinkEventfdFreeinfo,
            .Fstat = BlinkEventfdFstat,
            .Readv = BlinkEventfdReadv,
            .Writev = BlinkEventfdWritev,
            .Fcntl = BlinkEventfdFcntl,
            .Dup = BlinkEventfdDup,
            .Poll = BlinkEventfdPoll,
        },
};

static struct VfsDevice g_eventfddevice = {
    .mounts = NULL,
    .ops = &g_eventfdsystem.ops,
    .data = NULL,
    .dev = -1u,
    .refcount = 1u,
};

int BlinkEventfdCreate(unsigned initval, int flags) {
  int fd;
  struct VfsInfo *info;
  struct BlinkEventfdState *state;
  if (flags & ~(EFD_SEMAPHORE_LINUX | EFD_NONBLOCK_LINUX |
                EFD_CLOEXEC_LINUX)) {
    return einval();
  }
  if (!(state = calloc(1, sizeof(*state)))) {
    return enomem();
  }
  state->refcount = 1;
  state->counter = initval;
  state->oflags = O_RDWR | ((flags & EFD_NONBLOCK_LINUX) ? O_NDELAY : 0);
  state->semaphore = !!(flags & EFD_SEMAPHORE_LINUX);
  unassert(!pthread_mutex_init(&state->lock, 0));
  info = NULL;
  if (VfsCreateInfo(&info) == -1) {
    BlinkEventfdRelease(state);
    return -1;
  }
  info->data = state;
  info->parent = NULL;
  info->ino = (u64)(uintptr_t)state;
  info->dev = -1u;
  info->mode = S_IFIFO | 0600;
  info->refcount = 1;
  unassert(!VfsAcquireDevice(&g_eventfddevice, &info->device));
  fd = VfsAddFd(info);
  if (fd == -1) {
    unassert(!VfsFreeInfo(info));
    return -1;
  }
  if (BlinkDebugEventfdEnabled()) {
    fprintf(stderr, "[eventfd] create fd=%d flags=%#x init=%u\n", fd, flags,
            initval);
  }
  return fd;
}
