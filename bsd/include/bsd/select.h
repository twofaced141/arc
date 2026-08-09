#ifndef BSD_SELECT_H
#define BSD_SELECT_H

#include <stdint.h>
#include <string.h>

/* fd_set (up to FD_SETSIZE descriptors) */
typedef struct {
    uint64_t bits[16];   /* 16 * 64 = 1024 bits */
} fd_set_t;

#define FD_SETSIZE 1024

#define FD_ZERO(s)   memset((s), 0, sizeof(fd_set_t))
#define FD_SET(fd, s)  ((s)->bits[(fd) / 64] |=  (1ULL << ((fd) % 64)))
#define FD_CLR(fd, s)  ((s)->bits[(fd) / 64] &= ~(1ULL << ((fd) % 64)))
#define FD_ISSET(fd, s) ((fd) >= 0 && (fd) < FD_SETSIZE && \
                         ((s)->bits[(fd) / 64] & (1ULL << ((fd) % 64))))

/* poll(2) event / revents bits */
#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#endif
