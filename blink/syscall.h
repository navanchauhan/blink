#ifndef BLINK_SYSCALL_H_
#define BLINK_SYSCALL_H_
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "blink/builtin.h"
#include "blink/fds.h"
#include "blink/machine.h"
#include "blink/ndelay.h"
#include "blink/types.h"

#ifdef O_ASYNC
#define O_ASYNC_SETFL O_ASYNC
#else
#define O_ASYNC_SETFL 0
#endif
#ifdef O_DIRECT
#define O_DIRECT_SETFL O_DIRECT
#else
#define O_DIRECT_SETFL 0
#endif
#ifdef O_NOATIME
#define O_NOATIME_SETFL O_NOATIME
#else
#define O_NOATIME_SETFL 0
#endif

#define SETFL_FLAGS \
  (O_APPEND | O_NDELAY | O_ASYNC_SETFL | O_DIRECT_SETFL | O_NOATIME_SETFL)

#define INTERRUPTIBLE(restartable, x)       \
  do {                                      \
    int rc_;                                \
    rc_ = (x);                              \
    if (rc_ == -1 && errno == EINTR) {      \
      if (CheckInterrupt(m, restartable)) { \
        break;                              \
      }                                     \
    } else {                                \
      break;                                \
    }                                       \
  } while (1)

#define RESTARTABLE(x) INTERRUPTIBLE(true, x)

#define NORESTART(rc, x)             \
  do {                               \
    if (!CheckInterrupt(m, false)) { \
      INTERRUPTIBLE(false, rc = x);  \
    } else {                         \
      rc = -1;                       \
    }                                \
  } while (0)

extern char *g_blink_path;

void OpSyscall(P);

void SysCloseExec(struct System *);
int SysClose(struct Machine *, i32);
int SysCloseRange(struct Machine *, u32, u32, u32);
int SysDup(struct Machine *, i32, i32, i32, i32);
int SysOpenat(struct Machine *, i32, i64, i32, i32);
int SysPipe2(struct Machine *, i64, i32);
int SysIoctl(struct Machine *, int, u64, i64);
_Noreturn void SysExitGroup(struct Machine *, int);
_Noreturn void SysExit(struct Machine *, int);

bool OmniNoForkProcessHooksEnabled(void);
bool OmniNoForkIsPseudoProcess(struct Machine *);
bool OmniNoForkIsManagedThread(struct Machine *);
int OmniNoForkFork(struct Machine *, u64, u64, u64);
int OmniNoForkSpawnThread(struct Machine *, u64, u64, u64, u64, u64);
int OmniNoForkWait4(struct Machine *, int, int, int *, int *);
int OmniNoForkGetpid(struct Machine *);
void OmniNoForkPollState(struct Machine *);
int OmniNoForkGetppid(struct Machine *);
int OmniNoForkKill(struct Machine *, int, int);
int OmniNoForkTkill(struct Machine *, int, int);
int OmniNoForkTgkill(struct Machine *, int, int, int);
int OmniNoForkSetsid(struct Machine *);
int OmniNoForkGetsid(struct Machine *, int);
int OmniNoForkGetpgid(struct Machine *, int);
int OmniNoForkGetpgrp(struct Machine *);
int OmniNoForkSetpgid(struct Machine *, int, int);
int OmniNoForkTcgets(struct Machine *, int, struct termios *);
int OmniNoForkTcsets(struct Machine *, int, int, const struct termios *);
int OmniNoForkTcgetwinsize(struct Machine *, int, struct winsize *);
int OmniNoForkTcsetwinsize(struct Machine *, int, const struct winsize *);
int OmniNoForkTcgetsid(struct Machine *, int);
int OmniNoForkTcgetpgrp(struct Machine *, int);
int OmniNoForkTcsetpgrp(struct Machine *, int, int);
i64 OmniNoForkPtrace(struct Machine *, int, int, i64, i64);
_Noreturn void OmniNoForkExitSignal(struct Machine *, int);
_Noreturn void OmniNoForkExitThread(struct Machine *, int);
_Noreturn void OmniNoForkExitThreadGroup(struct Machine *, int);
_Noreturn void OmniNoForkExitGroup(struct Machine *, int);

int GetDirFildes(int);
void AddStdFd(struct Fds *, int);
int GetOflags(struct Machine *, int);
int GetFildes(struct Machine *, int);
struct Fd *GetAndLockFd(struct Machine *, int);
bool CheckInterrupt(struct Machine *, bool);
int SysStatfs(struct Machine *, i64, i64);
int SysFstatfs(struct Machine *, i32, i64);
int mkfifoat_(int, const char *, mode_t);
int mkfifo_(const char *, mode_t);

void Strace(struct Machine *, const char *, bool, const char *, ...);

#ifndef HAVE_MKFIFOAT
#ifdef mkfifoat
#undef mkfifoat
#endif
#define mkfifoat mkfifoat_
#endif

#ifndef HAVE_MKFIFO
#ifdef mkfifo
#undef mkfifo
#endif
#define mkfifo mkfifo_
#endif

#endif /* BLINK_SYSCALL_H_ */
