#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4
#define PROT_NONE    0x0

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_PHYS      0x200
#define MAP_POPULATE  0x8000
#define MAP_NORESERVE 0x4000
#define MAP_GROWSDOWN 0x0100
#define MAP_DENYWRITE 0x0800
#define MAP_EXECUTABLE 0x1000
#define MAP_LOCKED    0x2000
#define MAP_STACK     0x40000

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#define MCL_CURRENT   1
#define MCL_FUTURE    2
#define MCL_ONFAULT   4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t length, int flags);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);

#endif
