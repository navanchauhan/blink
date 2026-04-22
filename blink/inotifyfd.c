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
#include "blink/inotifyfd.h"

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
#include "blink/linux.h"
#include "blink/log.h"
#include "blink/macros.h"
#include "blink/syscall.h"
#include "blink/thread.h"
#include "blink/vfs.h"

struct BlinkInotifyWatch {
  int wd;
  char *path;
  u32 mask;
};

struct BlinkInotifyQueuedEvent {
  int wd;
  u32 mask;
  u32 cookie;
  char *name;
};

struct BlinkInotifyState {
  pthread_mutex_t_ lock;
  pthread_cond_t_ cond;
  _Atomic(u32) refcount;
  int oflags;
  int next_wd;
  struct BlinkInotifyWatch *watches;
  size_t watch_count;
  size_t watch_capacity;
  struct BlinkInotifyQueuedEvent *events;
  size_t event_count;
  size_t event_capacity;
  bool closed;
  struct BlinkInotifyState *registry_next;
};

struct inotify_event_linux_marshaled {
  u8 wd[4];
  u8 mask[4];
  u8 cookie[4];
  u8 len[4];
};

static pthread_mutex_t g_inotify_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct BlinkInotifyState *g_inotify_registry = NULL;
static _Atomic(u32) g_inotify_cookie = 1;

static ssize_t BlinkInotifyRead(struct VfsInfo *info, void *buf, size_t size);
static ssize_t BlinkInotifyReadv(struct VfsInfo *info, const struct iovec *iov,
                                 int iovcnt);
static int BlinkInotifyPoll(struct VfsInfo **infos, struct pollfd *fds,
                            nfds_t nfds, int timeout);

static size_t BlinkIovSize(const struct iovec *iov, int iovcnt) {
  size_t total = 0;
  int i;
  for (i = 0; i < iovcnt; ++i) {
    total += iov[i].iov_len;
  }
  return total;
}

static void BlinkIovCopyOutAt(const struct iovec *iov, int iovcnt,
                              size_t offset, const void *src, size_t size) {
  int i;
  size_t copied = 0;
  size_t consumed = 0;
  for (i = 0; i < iovcnt && copied < size; ++i) {
    if (offset >= consumed + iov[i].iov_len) {
      consumed += iov[i].iov_len;
      continue;
    }
    size_t iov_offset = offset > consumed ? offset - consumed : 0;
    size_t part = iov[i].iov_len - iov_offset;
    if (part > size - copied) {
      part = size - copied;
    }
    memcpy((char *)iov[i].iov_base + iov_offset, (const char *)src + copied,
           part);
    copied += part;
    offset += part;
    consumed += iov[i].iov_len;
  }
}

static size_t BlinkInotifyEventNameSize(const char *name) {
  if (name == NULL || !*name) {
    return 0;
  }
  return strlen(name) + 1;
}

static size_t BlinkInotifyEventSize(const struct BlinkInotifyQueuedEvent *event) {
  size_t namesize = BlinkInotifyEventNameSize(event->name);
  size_t padded = (namesize + 3u) & ~3u;
  return sizeof(struct inotify_event_linux_marshaled) + padded;
}

static void BlinkInotifyFreeEvent(struct BlinkInotifyQueuedEvent *event) {
  if (event == NULL) {
    return;
  }
  free(event->name);
  event->name = NULL;
}

static void BlinkInotifyUnregisterState(struct BlinkInotifyState *state) {
  struct BlinkInotifyState **it;
  pthread_mutex_lock(&g_inotify_registry_lock);
  for (it = &g_inotify_registry; *it; it = &(*it)->registry_next) {
    if (*it == state) {
      *it = state->registry_next;
      break;
    }
  }
  pthread_mutex_unlock(&g_inotify_registry_lock);
}

static struct BlinkInotifyState *BlinkInotifyAcquire(
    struct BlinkInotifyState *state) {
  int rc;
  rc = atomic_fetch_add(&state->refcount, 1);
  unassert(rc > 0);
  return state;
}

static int BlinkInotifyRelease(struct BlinkInotifyState *state) {
  size_t i;
  int rc;
  if (state == NULL) {
    return 0;
  }
  rc = atomic_fetch_sub(&state->refcount, 1);
  if (rc != 1) {
    return 0;
  }
  BlinkInotifyUnregisterState(state);
  for (i = 0; i < state->watch_count; ++i) {
    free(state->watches[i].path);
  }
  for (i = 0; i < state->event_count; ++i) {
    BlinkInotifyFreeEvent(&state->events[i]);
  }
  free(state->watches);
  free(state->events);
  unassert(!pthread_cond_destroy(&state->cond));
  unassert(!pthread_mutex_destroy(&state->lock));
  free(state);
  return 0;
}

static int BlinkInotifyFreeinfo(void *data) {
  return BlinkInotifyRelease((struct BlinkInotifyState *)data);
}

static int BlinkInotifyFstat(struct VfsInfo *info, struct stat *st) {
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

static int BlinkInotifyFcntl(struct VfsInfo *info, int cmd, va_list args) {
  struct BlinkInotifyState *state;
  int oflags;
  state = (struct BlinkInotifyState *)info->data;
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

static int BlinkInotifyDup(struct VfsInfo *info, struct VfsInfo **newinfo) {
  struct BlinkInotifyState *state;
  if (newinfo == NULL) {
    return efault();
  }
  *newinfo = NULL;
  if (VfsCreateInfo(newinfo) == -1) {
    return -1;
  }
  state = BlinkInotifyAcquire((struct BlinkInotifyState *)info->data);
  (*newinfo)->data = state;
  (*newinfo)->parent = NULL;
  (*newinfo)->ino = info->ino;
  (*newinfo)->dev = info->dev;
  (*newinfo)->mode = info->mode;
  (*newinfo)->refcount = 1;
  unassert(!VfsAcquireDevice(info->device, &(*newinfo)->device));
  return 0;
}

static struct VfsSystem g_inotifysystem = {
    .name = "inotify",
    .nodev = true,
    .ops =
        {
            .Freeinfo = BlinkInotifyFreeinfo,
            .Fstat = BlinkInotifyFstat,
            .Read = BlinkInotifyRead,
            .Readv = BlinkInotifyReadv,
            .Fcntl = BlinkInotifyFcntl,
            .Dup = BlinkInotifyDup,
            .Poll = BlinkInotifyPoll,
        },
};

static struct VfsDevice g_inotifydevice = {
    .mounts = NULL,
    .ops = &g_inotifysystem.ops,
    .data = NULL,
    .dev = -1u,
    .refcount = 1u,
};

static struct BlinkInotifyWatch *BlinkInotifyFindWatchByWd(
    struct BlinkInotifyState *state, int wd) {
  size_t i;
  for (i = 0; i < state->watch_count; ++i) {
    if (state->watches[i].wd == wd) {
      return state->watches + i;
    }
  }
  return NULL;
}

static struct BlinkInotifyWatch *BlinkInotifyFindWatchByPath(
    struct BlinkInotifyState *state, const char *path) {
  size_t i;
  for (i = 0; i < state->watch_count; ++i) {
    if (!strcmp(state->watches[i].path, path)) {
      return state->watches + i;
    }
  }
  return NULL;
}

static int BlinkInotifyEnsureWatchCapacity(struct BlinkInotifyState *state) {
  size_t newcap;
  struct BlinkInotifyWatch *watches;
  if (state->watch_count < state->watch_capacity) {
    return 0;
  }
  newcap = state->watch_capacity ? state->watch_capacity * 2 : 8;
  watches = realloc(state->watches, newcap * sizeof(*watches));
  if (watches == NULL) {
    return enomem();
  }
  state->watches = watches;
  state->watch_capacity = newcap;
  return 0;
}

static int BlinkInotifyEnsureEventCapacity(struct BlinkInotifyState *state) {
  size_t newcap;
  struct BlinkInotifyQueuedEvent *events;
  if (state->event_count < state->event_capacity) {
    return 0;
  }
  newcap = state->event_capacity ? state->event_capacity * 2 : 16;
  events = realloc(state->events, newcap * sizeof(*events));
  if (events == NULL) {
    return enomem();
  }
  state->events = events;
  state->event_capacity = newcap;
  return 0;
}

static int BlinkInotifyQueueEventLocked(struct BlinkInotifyState *state, int wd,
                                        u32 mask, u32 cookie,
                                        const char *name) {
  struct BlinkInotifyQueuedEvent *event;
  if (BlinkInotifyEnsureEventCapacity(state) == -1) {
    return -1;
  }
  event = state->events + state->event_count++;
  memset(event, 0, sizeof(*event));
  event->wd = wd;
  event->mask = mask;
  event->cookie = cookie;
  if (name && *name) {
    event->name = strdup(name);
    if (event->name == NULL) {
      --state->event_count;
      return enomem();
    }
  }
  pthread_cond_broadcast(&state->cond);
  return 0;
}

static int BlinkInotifyPollReadyLocked(struct BlinkInotifyState *state,
                                       short events) {
  short revents = 0;
  if ((events & POLLIN) && state->event_count) {
    revents |= POLLIN;
  }
  return revents;
}

static int BlinkInotifyPoll(struct VfsInfo **infos, struct pollfd *fds,
                            nfds_t nfds, int timeout) {
  struct BlinkInotifyState *state;
  struct timespec deadline;
  int ready;
  int rc;
  if (infos == NULL || fds == NULL || nfds != 1) {
    return einval();
  }
  state = (struct BlinkInotifyState *)infos[0]->data;
  fds[0].revents = 0;
  LOCK(&state->lock);
  if (timeout > 0 && !state->event_count) {
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
      UNLOCK(&state->lock);
      return -1;
    }
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec += deadline.tv_nsec / 1000000000L;
      deadline.tv_nsec %= 1000000000L;
    }
    while (!state->event_count && !state->closed) {
      rc = pthread_cond_timedwait(&state->cond, &state->lock, &deadline);
      if (rc == ETIMEDOUT) {
        break;
      }
      if (rc != 0) {
        break;
      }
    }
  }
  ready = BlinkInotifyPollReadyLocked(state, fds[0].events);
  fds[0].revents = ready;
  UNLOCK(&state->lock);
  return ready ? 1 : 0;
}

static ssize_t BlinkInotifyReadv(struct VfsInfo *info, const struct iovec *iov,
                                 int iovcnt) {
  struct BlinkInotifyState *state;
  size_t capacity;
  size_t offset;
  ssize_t total;
  state = (struct BlinkInotifyState *)info->data;
  capacity = BlinkIovSize(iov, iovcnt);
  total = 0;
  offset = 0;

  LOCK(&state->lock);
  while (!state->event_count && !(state->oflags & O_NDELAY)) {
    pthread_cond_wait(&state->cond, &state->lock);
  }
  if (!state->event_count) {
    UNLOCK(&state->lock);
    return eagain();
  }

  while (state->event_count) {
    struct BlinkInotifyQueuedEvent *event = state->events;
    struct inotify_event_linux_marshaled header;
    size_t namesize = BlinkInotifyEventNameSize(event->name);
    size_t padded = (namesize + 3u) & ~3u;
    size_t eventsize = sizeof(header) + padded;
    char *namebuf = NULL;

    if (!total && capacity < eventsize) {
      UNLOCK(&state->lock);
      return einval();
    }
    if (capacity - offset < eventsize) {
      break;
    }

    memset(&header, 0, sizeof(header));
    Write32(header.wd, event->wd);
    Write32(header.mask, event->mask);
    Write32(header.cookie, event->cookie);
    Write32(header.len, namesize);
    BlinkIovCopyOutAt(iov, iovcnt, offset, &header, sizeof(header));
    offset += sizeof(header);

    if (padded) {
      namebuf = calloc(1, padded);
      if (namebuf == NULL) {
        UNLOCK(&state->lock);
        return enomem();
      }
      if (event->name && namesize) {
        memcpy(namebuf, event->name, namesize);
      }
      BlinkIovCopyOutAt(iov, iovcnt, offset, namebuf, padded);
      free(namebuf);
      offset += padded;
    }

    total += (ssize_t)eventsize;
    BlinkInotifyFreeEvent(event);
    if (state->event_count > 1) {
      memmove(state->events, state->events + 1,
              (state->event_count - 1) * sizeof(*state->events));
    }
    --state->event_count;
  }

  UNLOCK(&state->lock);
  return total;
}

static ssize_t BlinkInotifyRead(struct VfsInfo *info, void *buf, size_t size) {
  struct iovec iov;
  iov.iov_base = buf;
  iov.iov_len = size;
  return BlinkInotifyReadv(info, &iov, 1);
}

static int BlinkGetInotifyState(int fd, struct BlinkInotifyState **out) {
  struct VfsInfo *info;
  struct BlinkInotifyState *state;
  if (out == NULL) {
    return efault();
  }
  *out = NULL;
  if (VfsGetFd(fd, &info) == -1) {
    return -1;
  }
  if (info->device != &g_inotifydevice) {
    unassert(!VfsFreeInfo(info));
    return einval();
  }
  state = BlinkInotifyAcquire((struct BlinkInotifyState *)info->data);
  unassert(!VfsFreeInfo(info));
  *out = state;
  return 0;
}

static int BlinkInotifyRegisterState(struct BlinkInotifyState *state) {
  pthread_mutex_lock(&g_inotify_registry_lock);
  state->registry_next = g_inotify_registry;
  g_inotify_registry = state;
  pthread_mutex_unlock(&g_inotify_registry_lock);
  return 0;
}

static int BlinkResolveWatchPath(const char *path, u32 mask, char **resolved) {
  struct VfsInfo *info;
  int rc;
  *resolved = NULL;
  if (VfsTraverse(path, &info, !(mask & IN_DONT_FOLLOW_LINUX)) == -1) {
    return -1;
  }
  if ((mask & IN_ONLYDIR_LINUX) && !S_ISDIR(info->mode)) {
    unassert(!VfsFreeInfo(info));
    return enotdir();
  }
  rc = VfsPathBuildFull(info, NULL, resolved) == -1 ? -1 : 0;
  unassert(!VfsFreeInfo(info));
  return rc;
}

static u32 BlinkInotifyNormalizeMask(u32 mask) {
  return mask & ~(IN_MASK_ADD_LINUX | IN_MASK_CREATE_LINUX | IN_DONT_FOLLOW_LINUX |
                  IN_ONLYDIR_LINUX | IN_EXCL_UNLINK_LINUX);
}

int BlinkInotifyInit(int flags) {
  int fd;
  struct VfsInfo *info;
  struct BlinkInotifyState *state;
  if (flags & ~(IN_CLOEXEC_LINUX | IN_NONBLOCK_LINUX)) {
    return einval();
  }
  state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return enomem();
  }
  state->refcount = 1;
  state->oflags = O_RDONLY | (flags & IN_NONBLOCK_LINUX ? O_NDELAY : 0);
  state->next_wd = 1;
  unassert(!pthread_mutex_init(&state->lock, 0));
  unassert(!pthread_cond_init(&state->cond, 0));
  if (VfsCreateInfo(&info) == -1) {
    BlinkInotifyRelease(state);
    return -1;
  }
  info->data = state;
  info->parent = NULL;
  info->ino = 0;
  info->dev = g_inotifydevice.dev;
  info->mode = S_IFIFO | 0600;
  info->refcount = 1;
  unassert(!VfsAcquireDevice(&g_inotifydevice, &info->device));
  BlinkInotifyRegisterState(state);
  fd = VfsAddFd(info);
  if (fd == -1) {
    BlinkInotifyRelease(state);
    return -1;
  }
  if (flags & IN_CLOEXEC_LINUX) {
    unassert(!VfsFcntl(fd, F_SETFD, FD_CLOEXEC));
  }
  return fd;
}

int BlinkInotifyAddWatch(int fd, const char *path, u32 mask) {
  struct BlinkInotifyState *state;
  struct BlinkInotifyWatch *watch;
  char *resolved;
  int wd;
  if (path == NULL) {
    return efault();
  }
  if (BlinkGetInotifyState(fd, &state) == -1) {
    return -1;
  }
  if (BlinkResolveWatchPath(path, mask, &resolved) == -1) {
    BlinkInotifyRelease(state);
    return -1;
  }

  LOCK(&state->lock);
  watch = BlinkInotifyFindWatchByPath(state, resolved);
  if (watch != NULL) {
    if (mask & IN_MASK_CREATE_LINUX) {
      UNLOCK(&state->lock);
      free(resolved);
      BlinkInotifyRelease(state);
      return eexist();
    }
    if (mask & IN_MASK_ADD_LINUX) {
      watch->mask |= BlinkInotifyNormalizeMask(mask);
    } else {
      watch->mask = BlinkInotifyNormalizeMask(mask);
    }
    wd = watch->wd;
    UNLOCK(&state->lock);
    free(resolved);
    BlinkInotifyRelease(state);
    return wd;
  }

  if (BlinkInotifyEnsureWatchCapacity(state) == -1) {
    UNLOCK(&state->lock);
    free(resolved);
    BlinkInotifyRelease(state);
    return -1;
  }

  watch = state->watches + state->watch_count++;
  memset(watch, 0, sizeof(*watch));
  watch->wd = state->next_wd++;
  watch->path = resolved;
  watch->mask = BlinkInotifyNormalizeMask(mask);
  wd = watch->wd;
  UNLOCK(&state->lock);
  BlinkInotifyRelease(state);
  return wd;
}

int BlinkInotifyRmWatch(int fd, int wd) {
  struct BlinkInotifyState *state;
  struct BlinkInotifyWatch *watch;
  size_t index;
  if (BlinkGetInotifyState(fd, &state) == -1) {
    return -1;
  }

  LOCK(&state->lock);
  watch = BlinkInotifyFindWatchByWd(state, wd);
  if (watch == NULL) {
    UNLOCK(&state->lock);
    BlinkInotifyRelease(state);
    return einval();
  }

  index = (size_t)(watch - state->watches);
  BlinkInotifyQueueEventLocked(state, watch->wd, IN_IGNORED_LINUX, 0, NULL);
  free(watch->path);
  if (index + 1 < state->watch_count) {
    memmove(state->watches + index, state->watches + index + 1,
            (state->watch_count - index - 1) * sizeof(*state->watches));
  }
  --state->watch_count;
  UNLOCK(&state->lock);
  BlinkInotifyRelease(state);
  return 0;
}

static void BlinkSplitParentAndName(const char *path, char **parent,
                                    const char **name) {
  char *slash;
  *parent = NULL;
  *name = NULL;
  if (path == NULL || path[0] != '/') {
    return;
  }
  slash = strrchr(path, '/');
  if (slash == NULL) {
    return;
  }
  *name = slash[1] ? slash + 1 : "";
  if (slash == path) {
    *parent = strdup("/");
    return;
  }
  *parent = strndup(path, slash - path);
}

static void BlinkDispatchExactLocked(struct BlinkInotifyState *state,
                                     const char *path, u32 mask, u32 cookie) {
  size_t i;
  u32 event_mask = mask & ~IN_ISDIR_LINUX;
  if (!path || !mask || !event_mask) {
    return;
  }
  for (i = 0; i < state->watch_count; ++i) {
    if (!strcmp(state->watches[i].path, path) &&
        (state->watches[i].mask & event_mask)) {
      BlinkInotifyQueueEventLocked(state, state->watches[i].wd, mask, cookie,
                                   NULL);
    }
  }
}

static void BlinkDispatchParentLocked(struct BlinkInotifyState *state,
                                      const char *parent, const char *name,
                                      u32 mask, u32 cookie) {
  size_t i;
  u32 event_mask = mask & ~IN_ISDIR_LINUX;
  if (!parent || !name || !mask || !event_mask) {
    return;
  }
  for (i = 0; i < state->watch_count; ++i) {
    if (!strcmp(state->watches[i].path, parent) &&
        (state->watches[i].mask & event_mask)) {
      BlinkInotifyQueueEventLocked(state, state->watches[i].wd, mask, cookie,
                                   name);
    }
  }
}

void BlinkInotifyNotifyPathEvent(const char *path, u32 self_mask,
                                 u32 parent_mask) {
  struct BlinkInotifyState *state;
  char *parent = NULL;
  const char *name = NULL;
  if (path == NULL || path[0] != '/') {
    return;
  }
  BlinkSplitParentAndName(path, &parent, &name);
  pthread_mutex_lock(&g_inotify_registry_lock);
  for (state = g_inotify_registry; state; state = state->registry_next) {
    LOCK(&state->lock);
    BlinkDispatchExactLocked(state, path, self_mask, 0);
    BlinkDispatchParentLocked(state, parent, name, parent_mask, 0);
    UNLOCK(&state->lock);
  }
  pthread_mutex_unlock(&g_inotify_registry_lock);
  free(parent);
}

void BlinkInotifyNotifyMoveEvent(const char *old_path, const char *new_path,
                                 u32 self_mask, u32 old_parent_mask,
                                 u32 new_parent_mask) {
  struct BlinkInotifyState *state;
  char *old_parent = NULL;
  char *new_parent = NULL;
  const char *old_name = NULL;
  const char *new_name = NULL;
  u32 cookie;
  if (old_path == NULL || new_path == NULL || old_path[0] != '/' ||
      new_path[0] != '/') {
    return;
  }
  cookie = atomic_fetch_add(&g_inotify_cookie, 1);
  BlinkSplitParentAndName(old_path, &old_parent, &old_name);
  BlinkSplitParentAndName(new_path, &new_parent, &new_name);
  pthread_mutex_lock(&g_inotify_registry_lock);
  for (state = g_inotify_registry; state; state = state->registry_next) {
    LOCK(&state->lock);
    BlinkDispatchExactLocked(state, old_path, self_mask, cookie);
    BlinkDispatchParentLocked(state, old_parent, old_name, old_parent_mask,
                              cookie);
    BlinkDispatchParentLocked(state, new_parent, new_name, new_parent_mask,
                              cookie);
    UNLOCK(&state->lock);
  }
  pthread_mutex_unlock(&g_inotify_registry_lock);
  free(old_parent);
  free(new_parent);
}
