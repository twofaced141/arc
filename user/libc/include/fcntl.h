#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_ACCMODE    3
#define O_CREAT      0x40
#define O_EXCL       0x80
#define O_NOCTTY     0x100
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define O_NONBLOCK   0x800
#define O_NDELAY    O_NONBLOCK
#define O_DSYNC      0x1000
#define O_DIRECT     0x4000
#define O_LARGEFILE  0x8000
#define O_DIRECTORY  0x10000
#define O_NOFOLLOW   0x20000
#define O_NOATIME    0x40000
#ifndef O_CLOEXEC
#define O_CLOEXEC    0x80000
#endif
#define O_SYNC       0x101000
#define O_RSYNC      O_SYNC
#define O_PATH       0x100000
#define O_TMPFILE    0x202000

#define F_DUPFD       0
#define F_GETFD       1
#define F_SETFD       2
#define F_GETFL       3
#define F_SETFL       4
#define F_GETLK       5
#define F_SETLK       6
#define F_SETLKW      7
#define F_DUPFD_CLOEXEC 1030
#define F_GETPIPE_SZ  1032
#define F_SETPIPE_SZ  1031

#define FD_CLOEXEC    1

#ifndef AT_FDCWD
#define AT_FDCWD             (-100)
#endif
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200
#define AT_EACCESS           0x200

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif
