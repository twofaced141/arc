#ifndef BSD_ERRNO_H
#define BSD_ERRNO_H

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ETXTBSY 26
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32
#define EDOM    33
#define ERANGE  34

#define ENOSYS  38
#define ELOOP   40
#define EBUSY   16
#define ENAMETOOLONG 63
#define ENOTEMPTY 66

#define EWOULDBLOCK EAGAIN
#define ETIMEDOUT  110
#define EDEADLK    35

/* Internal pseudo-error: an interruptible syscall was blocked when a
 * signal arrived.  The dispatcher restarts the syscall if the delivered
 * signal has SA_RESTART, otherwise converts it to EINTR. */
#define ERESTARTSYS 512

extern int bsd_errno;

#endif
