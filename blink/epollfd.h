#ifndef BLINK_EPOLLFD_H_
#define BLINK_EPOLLFD_H_

#include <time.h>

#include "blink/machine.h"
#include "blink/types.h"

struct epoll_event_linux;

int BlinkEpollCreate(int flags);
int BlinkEpollCtl(int epfd, int op, int fd, u32 events, u64 data);
int BlinkEpollWait(struct Machine *m, int epfd,
                   struct epoll_event_linux *events, int maxevents,
                   struct timespec deadline);

#endif /* BLINK_EPOLLFD_H_ */
