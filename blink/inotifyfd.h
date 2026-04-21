#ifndef BLINK_INOTIFYFD_H_
#define BLINK_INOTIFYFD_H_

#include "blink/linux.h"
#include "blink/types.h"

#define IN_ACCESS_LINUX        0x00000001u
#define IN_MODIFY_LINUX        0x00000002u
#define IN_ATTRIB_LINUX        0x00000004u
#define IN_CLOSE_WRITE_LINUX   0x00000008u
#define IN_CLOSE_NOWRITE_LINUX 0x00000010u
#define IN_OPEN_LINUX          0x00000020u
#define IN_MOVED_FROM_LINUX    0x00000040u
#define IN_MOVED_TO_LINUX      0x00000080u
#define IN_CREATE_LINUX        0x00000100u
#define IN_DELETE_LINUX        0x00000200u
#define IN_DELETE_SELF_LINUX   0x00000400u
#define IN_MOVE_SELF_LINUX     0x00000800u
#define IN_UNMOUNT_LINUX       0x00002000u
#define IN_Q_OVERFLOW_LINUX    0x00004000u
#define IN_IGNORED_LINUX       0x00008000u
#define IN_ONLYDIR_LINUX       0x01000000u
#define IN_DONT_FOLLOW_LINUX   0x02000000u
#define IN_EXCL_UNLINK_LINUX   0x04000000u
#define IN_MASK_CREATE_LINUX   0x10000000u
#define IN_MASK_ADD_LINUX      0x20000000u
#define IN_ISDIR_LINUX         0x40000000u
#define IN_ONESHOT_LINUX       0x80000000u

#define IN_CLOEXEC_LINUX  O_CLOEXEC_LINUX
#define IN_NONBLOCK_LINUX O_NDELAY_LINUX

int BlinkInotifyInit(int flags);
int BlinkInotifyAddWatch(int fd, const char *path, u32 mask);
int BlinkInotifyRmWatch(int fd, int wd);
void BlinkInotifyNotifyPathEvent(const char *path, u32 self_mask,
                                 u32 parent_mask);
void BlinkInotifyNotifyMoveEvent(const char *old_path, const char *new_path,
                                 u32 self_mask, u32 old_parent_mask,
                                 u32 new_parent_mask);

#endif /* BLINK_INOTIFYFD_H_ */
