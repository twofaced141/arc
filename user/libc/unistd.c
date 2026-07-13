#include <unistd.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
struct sigaction;

// optind/opterr/optopt/optarg provided by GLIBC when linked with -lc

#define SYSCALL_GETPID   0
#define SYSCALL_PUTC     1
#define SYSCALL_YIELD    2
#define SYSCALL_EXIT     3
#define SYSCALL_WRITE    4
#define SYSCALL_READ     5
#define SYSCALL_SLEEP    6
#define SYSCALL_GETTICKS 7
#define SYSCALL_OPEN     8
#define SYSCALL_CLOSE    9
#define SYSCALL_LSEEK    10
#define SYSCALL_FSTAT    11
#define SYSCALL_BRK      12
#define SYSCALL_SBRK     13
#define SYSCALL_FORK     14
#define SYSCALL_EXECVE   15
#define SYSCALL_WAITPID  16
#define SYSCALL_CHDIR    17
#define SYSCALL_GETCWD   18
#define SYSCALL_LISTDIR  19
#define SYSCALL_KILL     20
#define SYSCALL_DUP2     21
#define SYSCALL_PIPE     22
#define SYSCALL_IOCTL    23
#define SYSCALL_GETTIME  24
#define SYSCALL_SIGACTION 25
#define SYSCALL_SIGRETURN 26
#define SYSCALL_UNAME    28
#define SYSCALL_MMAP     29
#define SYSCALL_MUNMAP   30
#define SYSCALL_STAT     31
#define SYSCALL_LSTAT    32
#define SYSCALL_GETDENTS 33
#define SYSCALL_UNLINK   34
#define SYSCALL_RENAME   35
#define SYSCALL_MKDIR    36
#define SYSCALL_RMDIR    37
#define SYSCALL_CHMOD    38
#define SYSCALL_CHOWN    39
#define SYSCALL_ACCESS   40
#define SYSCALL_GETUID   41
#define SYSCALL_GETGID   42
#define SYSCALL_GETEUID    43
#define SYSCALL_GETEGID    44
#define SYSCALL_SIGPROCMASK 45
#define SYSCALL_SIGPENDING  46
#define SYSCALL_SIGSUSPEND  47
#define SYSCALL_READLINK   48
#define SYSCALL_LINK       49
#define SYSCALL_TRUNCATE   50
#define SYSCALL_FTRUNCATE  51
#define SYSCALL_ALARM      52
#define SYSCALL_SYMLINK    53
#define SYSCALL_FCHDIR     54
#define SYSCALL_FCHMOD     55
#define SYSCALL_FCHOWN     56
#define SYSCALL_DUP        57
#define SYSCALL_SETUID     58
#define SYSCALL_SETGID     59
#define SYSCALL_SETEUID    60
#define SYSCALL_SETEGID    61
#define SYSCALL_GETPPID    62
#define SYSCALL_PAUSE      63
#define SYSCALL_FSYNC      64
#define SYSCALL_FDATASYNC  65
#define SYSCALL_NICE       66
#define SYSCALL_GETPRIORITY 67
#define SYSCALL_SETPRIORITY 68
#define SYSCALL_UTIMENSAT  69
#define SYSCALL_UMASK      70
#define SYSCALL_REBOOT      71
#define SYSCALL_SETHOSTNAME 72
#define SYSCALL_GETHOSTNAME 73
#define SYSCALL_SETPGID     74
#define SYSCALL_GETPGID     75
#define SYSCALL_TCSETPGRP   76
#define SYSCALL_TCGETPGRP   77
#define SYSCALL_POWEROFF    78
#define SYSCALL_SOCKET      79
#define SYSCALL_BIND        80
#define SYSCALL_CONNECT     81
#define SYSCALL_LISTEN      82
#define SYSCALL_ACCEPT      83
#define SYSCALL_SEND        84
#define SYSCALL_RECV        85
#define SYSCALL_GETFBINFO   87
#define SYSCALL_GETMOUSE    88
#define SYSCALL_OOM_KILL    89
#define SYSCALL_MAP_FB      90
#define SYSCALL_FB_SET_ACTIVE 91

static inline int do_syscall(int num, int a, int b, int c, int d) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory");
    return ret;
}

int read(int fd, void *buf, size_t count) {
    return do_syscall(SYSCALL_READ, fd, (int)buf, (int)count, 0);
}

int write(int fd, const void *buf, size_t count) {
    return do_syscall(SYSCALL_WRITE, fd, (int)buf, (int)count, 0);
}

int open(const char *path, int flags, ...) {
    return do_syscall(SYSCALL_OPEN, (int)path, flags, 0, 0);
}

int close(int fd) {
    return do_syscall(SYSCALL_CLOSE, fd, 0, 0, 0);
}

pid_t fork(void) {
    return do_syscall(SYSCALL_FORK, 0, 0, 0, 0);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    (void)envp;
    if (!argv || !argv[0]) {
        return do_syscall(SYSCALL_EXECVE, (int)path, 0, 0, 0);
    }
    char flat[256];
    int pos = 0;
    for (int i = 0; argv[i] && pos < 255; i++) {
        const char *s = argv[i];
        while (*s && pos < 255)
            flat[pos++] = *s++;
        if (argv[i + 1] && pos < 255)
            flat[pos++] = ' ';
    }
    flat[pos] = '\0';
    return do_syscall(SYSCALL_EXECVE, (int)path, (int)flat, 0, 0);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    int st;
    int r = do_syscall(SYSCALL_WAITPID, pid, (int)&st, options, 0);
    if (status) *status = st;
    return r;
}

pid_t wait3(int *status, int options, void *rusage) {
    (void)rusage;
    return waitpid(-1, status, options);
}

pid_t wait4(pid_t pid, int *status, int options, void *rusage) {
    (void)rusage;
    return waitpid(pid, status, options);
}

unsigned int sleep(unsigned int seconds) {
    do_syscall(SYSCALL_SLEEP, (int)(seconds * 100), 0, 0, 0);
    return 0;
}

int brk(void *addr) {
    return do_syscall(SYSCALL_BRK, (int)addr, 0, 0, 0);
}

void *sbrk(intptr_t increment) {
    return (void *)(intptr_t)do_syscall(SYSCALL_SBRK, (int)increment, 0, 0, 0);
}

pid_t getpid(void) {
    return do_syscall(SYSCALL_GETPID, 0, 0, 0, 0);
}

pid_t setsid(void) {
    return -1;
}

pid_t getsid(pid_t pid) {
    (void)pid;
    return -1;
}

char *getcwd(char *buf, size_t size) {
    int n = do_syscall(SYSCALL_GETCWD, (int)buf, (int)size, 0, 0);
    if (n < 0) return NULL;
    return buf;
}

int chdir(const char *path) {
    return do_syscall(SYSCALL_CHDIR, (int)path, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    return do_syscall(SYSCALL_LSEEK, fd, offset, whence, 0);
}

int fstat(int fd, struct stat *buf) {
    return do_syscall(SYSCALL_FSTAT, fd, (int)buf, 0, 0);
}

int dup2(int oldfd, int newfd) {
    return do_syscall(SYSCALL_DUP2, oldfd, newfd, 0, 0);
}

int pipe(int pipefd[2]) {
    return do_syscall(SYSCALL_PIPE, (int)pipefd, 0, 0, 0);
}

int ioctl(int fd, int request, ...) {
    void *arg;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    return do_syscall(SYSCALL_IOCTL, fd, request, (int)arg, 0);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    switch (cmd) {
        case F_DUPFD: {
            int arg = va_arg(ap, int);
            va_end(ap);
            while (1) {
                int nfd = dup(fd);
                if (nfd < 0) return -1;
                if (nfd >= arg) return nfd;
                close(nfd);
            }
        }
        case F_GETFD:
            va_end(ap);
            return 0;
        case F_SETFD:
            va_end(ap);
            return 0;
        case F_GETFL:
            va_end(ap);
            return 0;
        default:
            va_end(ap);
            errno = EINVAL;
            return -1;
    }
}

int kill(pid_t pid, int sig) {
    return do_syscall(SYSCALL_KILL, pid, sig, 0, 0);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void *ret;
    int off = (int)offset;
    __asm__ __volatile__(
        "push %%ebp\n"
        "mov %7, %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(ret)
        : "a"(SYSCALL_MMAP), "b"(addr), "c"(length), "d"(prot), "S"(flags), "D"(fd), "m"(off)
        : "memory");
    if ((intptr_t)ret == -1) return (void *)-1;
    return ret;
}

int munmap(void *addr, size_t length) {
    return do_syscall(SYSCALL_MUNMAP, (int)addr, (int)length, 0, 0);
}

int mlockall(int flags) {
    (void)flags;
    return 0;
}

int uname(struct utsname *buf) {
    return do_syscall(SYSCALL_UNAME, (int)buf, 0, 0, 0);
}

int yield(void) {
    return do_syscall(SYSCALL_YIELD, 0, 0, 0, 0);
}

int getticks(void) {
    return do_syscall(SYSCALL_GETTICKS, 0, 0, 0, 0);
}

int gettime(struct rtc_time *t) {
    return do_syscall(SYSCALL_GETTIME, (int)t, 0, 0, 0);
}

int stat(const char *path, struct stat *buf) {
    return do_syscall(SYSCALL_STAT, (int)path, (int)buf, 0, 0);
}

int lstat(const char *path, struct stat *buf) {
    return do_syscall(SYSCALL_LSTAT, (int)path, (int)buf, 0, 0);
}

int getdents(const char *path, void *dirp, unsigned int count) {
    return do_syscall(SYSCALL_GETDENTS, (int)path, (int)dirp, (int)count, 0);
}

int unlink(const char *path) {
    return do_syscall(SYSCALL_UNLINK, (int)path, 0, 0, 0);
}

int rename(const char *oldpath, const char *newpath) {
    return do_syscall(SYSCALL_RENAME, (int)oldpath, (int)newpath, 0, 0);
}

int mkdir(const char *path, unsigned int mode) {
    return do_syscall(SYSCALL_MKDIR, (int)path, (int)mode, 0, 0);
}

int rmdir(const char *path) {
    return do_syscall(SYSCALL_RMDIR, (int)path, 0, 0, 0);
}

int chmod(const char *path, unsigned int mode) {
    return do_syscall(SYSCALL_CHMOD, (int)path, (int)mode, 0, 0);
}

int chown(const char *path, unsigned int owner, unsigned int group) {
    return do_syscall(SYSCALL_CHOWN, (int)path, (int)owner, (int)group, 0);
}

int access(const char *path, int mode) {
    return do_syscall(SYSCALL_ACCESS, (int)path, (int)mode, 0, 0);
}

unsigned int getuid(void) {
    return (unsigned int)do_syscall(SYSCALL_GETUID, 0, 0, 0, 0);
}

unsigned int getgid(void) {
    return (unsigned int)do_syscall(SYSCALL_GETGID, 0, 0, 0, 0);
}

unsigned int geteuid(void) {
    return (unsigned int)do_syscall(SYSCALL_GETEUID, 0, 0, 0, 0);
}

unsigned int getegid(void) {
    return (unsigned int)do_syscall(SYSCALL_GETEGID, 0, 0, 0, 0);
}

int sigprocmask(int how, const uint32_t *set, uint32_t *oldset) {
    return do_syscall(SYSCALL_SIGPROCMASK, how, (int)set, (int)oldset, 0);
}

int sigpending(uint32_t *set) {
    return do_syscall(SYSCALL_SIGPENDING, (int)set, 0, 0, 0);
}

int sigsuspend(const uint32_t *mask) {
    return do_syscall(SYSCALL_SIGSUSPEND, (int)mask, 0, 0, 0);
}

int readlink(const char *path, char *buf, size_t bufsiz) {
    return do_syscall(SYSCALL_READLINK, (int)path, (int)buf, (int)bufsiz, 0);
}

int link(const char *oldpath, const char *newpath) {
    return do_syscall(SYSCALL_LINK, (int)oldpath, (int)newpath, 0, 0);
}

int truncate(const char *path, off_t length) {
    (void)length;
    return do_syscall(SYSCALL_TRUNCATE, (int)path, 0, 0, 0);
}

int ftruncate(int fd, off_t length) {
    (void)length;
    return do_syscall(SYSCALL_FTRUNCATE, fd, 0, 0, 0);
}

int posix_fallocate(int fd, off_t offset, off_t len) {
    (void)fd; (void)offset; (void)len;
    errno = ENOSYS;
    return -1;
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)do_syscall(SYSCALL_ALARM, (int)seconds, 0, 0, 0);
}

int symlink(const char *target, const char *linkpath) {
    return do_syscall(SYSCALL_SYMLINK, (int)target, (int)linkpath, 0, 0);
}

int fchdir(int fd) {
    return do_syscall(SYSCALL_FCHDIR, fd, 0, 0, 0);
}

int fchmod(int fd, unsigned int mode) {
    return do_syscall(SYSCALL_FCHMOD, fd, (int)mode, 0, 0);
}

int fchown(int fd, unsigned int owner, unsigned int group) {
    return do_syscall(SYSCALL_FCHOWN, fd, (int)owner, (int)group, 0);
}

int dup(int oldfd) {
    return do_syscall(SYSCALL_DUP, oldfd, 0, 0, 0);
}

int setuid(unsigned int uid) {
    return do_syscall(SYSCALL_SETUID, (int)uid, 0, 0, 0);
}

int setgid(unsigned int gid) {
    return do_syscall(SYSCALL_SETGID, (int)gid, 0, 0, 0);
}

int getgroups(int size, unsigned int *list) {
    (void)size; (void)list;
    return 0;
}

int setgroups(size_t size, const unsigned int *list) {
    (void)size; (void)list;
    return -1;
}

int seteuid(unsigned int euid) {
    return do_syscall(SYSCALL_SETEUID, (int)euid, 0, 0, 0);
}

int setegid(unsigned int egid) {
    return do_syscall(SYSCALL_SETEGID, (int)egid, 0, 0, 0);
}

pid_t getppid(void) {
    return (pid_t)do_syscall(SYSCALL_GETPPID, 0, 0, 0, 0);
}

int pause(void) {
    return do_syscall(SYSCALL_PAUSE, 0, 0, 0, 0);
}

int fsync(int fd) {
    return do_syscall(SYSCALL_FSYNC, fd, 0, 0, 0);
}

int fdatasync(int fd) {
    return do_syscall(SYSCALL_FDATASYNC, fd, 0, 0, 0);
}

void sync(void) {
}

int nice(int inc) {
    return do_syscall(SYSCALL_NICE, inc, 0, 0, 0);
}

int getpriority(int which, int who) {
    return do_syscall(SYSCALL_GETPRIORITY, which, who, 0, 0);
}

int setpriority(int which, int who, int prio) {
    return do_syscall(SYSCALL_SETPRIORITY, which, who, prio, 0);
}

int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags) {
    (void)dirfd;
    (void)flags;
    return do_syscall(SYSCALL_UTIMENSAT, (int)pathname, (int)times, 0, 0);
}

unsigned int umask(unsigned int mask) {
    return (unsigned int)do_syscall(SYSCALL_UMASK, (int)mask, 0, 0, 0);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return do_syscall(SYSCALL_SIGACTION, signum, (int)act, (int)oldact, 0);
}

int reboot(int cmd) {
    (void)cmd;
    return do_syscall(SYSCALL_REBOOT, 0, 0, 0, 0);
}

int poweroff(void) {
    return do_syscall(SYSCALL_POWEROFF, 0, 0, 0, 0);
}

ssize_t readahead(int fd, off64_t offset, size_t count) {
    (void)fd; (void)offset; (void)count;
    return -1;
}

int sethostname(const char *name, size_t len) {
    (void)len;
    return do_syscall(SYSCALL_SETHOSTNAME, (int)name, 0, 0, 0);
}

int gethostname(char *name, size_t len) {
    return do_syscall(SYSCALL_GETHOSTNAME, (int)name, (int)len, 0, 0);
}

int setpgid(int pid, int pgid) {
    return do_syscall(SYSCALL_SETPGID, pid, pgid, 0, 0);
}

int getpgid(int pid) {
    return do_syscall(SYSCALL_GETPGID, pid, 0, 0, 0);
}

int tcsetpgrp(int fd, int pgrp) {
    return do_syscall(SYSCALL_TCSETPGRP, fd, pgrp, 0, 0);
}

int tcgetpgrp(int fd) {
    return do_syscall(SYSCALL_TCGETPGRP, fd, 0, 0, 0);
}

int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int action, const struct termios *t) {
    (void)action;
    return ioctl(fd, TCSETS, (void *)t);
}

// ----- exec convenience wrappers -----

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/'))
        return execve(file, argv, environ);
    const char *path = getenv("PATH");
    if (!path) path = "/bin";
    char buf[PATH_MAX];
    while (*path) {
        const char *next = strchr(path, ':');
        size_t dlen = next ? (size_t)(next - path) : strlen(path);
        size_t flen = strlen(file);
        if (dlen + 1 + flen >= sizeof(buf)) {
            path = next ? next + 1 : path + dlen;
            continue;
        }
        memcpy(buf, path, dlen);
        buf[dlen] = '/';
        memcpy(buf + dlen + 1, file, flen + 1);
        execve(buf, argv, environ);
        if (errno != ENOENT && errno != EACCES) return -1;
        path = next ? next + 1 : path + dlen;
    }
    errno = ENOENT;
    return -1;
}

int execl(const char *path, const char *arg, ...) {
    const char *args[64];
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    args[n++] = arg;
    while (n < 63) {
        const char *a = va_arg(ap, const char *);
        args[n++] = a;
        if (!a) break;
    }
    va_end(ap);
    return execv(path, (char *const *)args);
}

int execlp(const char *file, const char *arg, ...) {
    const char *args[64];
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    args[n++] = arg;
    while (n < 63) {
        const char *a = va_arg(ap, const char *);
        args[n++] = a;
        if (!a) break;
    }
    va_end(ap);
    return execvp(file, (char *const *)args);
}

int execle(const char *path, const char *arg, ...) {
    const char *args[64];
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    args[n++] = arg;
    while (n < 63) {
        const char *a = va_arg(ap, const char *);
        args[n++] = a;
        if (!a) break;
    }
    va_end(ap);
    /* The last non-NULL arg is envp; skip it for now */
    return execv(path, (char *const *)args);
}

void _exit(int status) {
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(3), "b"(status), "c"(0), "d"(0), "S"(0));
    for (;;);
}

// ----- getpagesize -----

int getpagesize(void) { return 4096; }

// ----- sysconf -----

long sysconf(int name) {
    switch (name) {
        case _SC_ARG_MAX: return ARG_MAX;
        case _SC_PAGESIZE: return 4096;
        case _SC_NPROCESSORS_ONLN: return 1;
        case _SC_OPEN_MAX: return OPEN_MAX;
        case _SC_CLK_TCK: return 100;
        case _SC_HOST_NAME_MAX: return HOST_NAME_MAX;
        case _SC_PIPE_BUF: return PIPE_BUF;
        default: errno = EINVAL; return -1;
    }
}

long pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
        case _PC_NAME_MAX: return 255;
        default: errno = EINVAL; return -1;
    }
}

// ----- *at functions -----

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return open(pathname, flags);
    errno = ENOSYS;
    return -1;
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    if (dirfd == AT_FDCWD || pathname[0] == '/') {
        if (flags & AT_SYMLINK_NOFOLLOW)
            return lstat(pathname, buf);
        return stat(pathname, buf);
    }
    errno = ENOSYS;
    return -1;
}

int readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return readlink(pathname, buf, bufsiz);
    errno = ENOSYS;
    return -1;
}

int mkdirat(int dirfd, const char *pathname, unsigned int mode) {
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return mkdir(pathname, mode);
    errno = ENOSYS;
    return -1;
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    (void)flags;
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return unlink(pathname);
    errno = ENOSYS;
    return -1;
}

int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {
    (void)flags;
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return chmod(pathname, mode);
    errno = ENOSYS;
    return -1;
}

int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags) {
    (void)flags;
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return chown(pathname, owner, group);
    errno = ENOSYS;
    return -1;
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    (void)flags;
    if (dirfd == AT_FDCWD || pathname[0] == '/')
        return access(pathname, mode);
    errno = ENOSYS;
    return -1;
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    (void)flags;
    if ((olddirfd == AT_FDCWD || oldpath[0] == '/') &&
        (newdirfd == AT_FDCWD || newpath[0] == '/'))
        return link(oldpath, newpath);
    errno = ENOSYS;
    return -1;
}

int symlinkat(const char *target, int newdirfd, const char *linkpath) {
    if (newdirfd == AT_FDCWD || linkpath[0] == '/')
        return symlink(target, linkpath);
    errno = ENOSYS;
    return -1;
}

int mknod(const char *path, unsigned int mode, unsigned int dev) {
    (void)path; (void)mode; (void)dev;
    errno = ENOSYS;
    return -1;
}

int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev) {
    (void)dirfd; (void)pathname; (void)mode; (void)dev;
    errno = ENOSYS;
    return -1;
}

int lchown(const char *pathname, uid_t owner, gid_t group) {
    return chown(pathname, owner, group);
}

int futimens(int fd, const struct timespec times[2]) {
    (void)fd; (void)times;
    return -1;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tv; (void)tz;
    return -1;
}

int statvfs(const char *path, struct statvfs *buf) {
    (void)path; (void)buf;
    return -1;
}

int fstatvfs(int fd, struct statvfs *buf) {
    (void)fd; (void)buf;
    return -1;
}

char *nl_langinfo(int item) {
    (void)item;
    return "";
}

size_t confstr(int name, char *buf, size_t len) {
    (void)name;
    if (buf && len > 0) buf[0] = '\0';
    return 0;
}

int isatty(int fd) {
    struct { unsigned short row, col, xpix, ypix; } ws;
    return ioctl(fd, TIOCGWINSZ, &ws) == 0;
}

char *ttyname(int fd) {
    (void)fd;
    return NULL;
}

int tcflush(int fd, int queue) {
    (void)queue;
    errno = ENOSYS;
    return -1;
}

long syscall(long number, ...) {
    (void)number;
    errno = ENOSYS;
    return -1;
}

int cfsetspeed(struct termios *tio, speed_t speed) {
    (void)tio;
    (void)speed;
    return 0;
}

void cfmakeraw(struct termios *tio) {
    tio->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                      ICRNL | IXON);
    tio->c_oflag &= ~OPOST;
    tio->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio->c_cflag &= ~(CSIZE | PARENB);
    tio->c_cflag |= CS8;
    tio->c_cc[VMIN] = 1;
    tio->c_cc[VTIME] = 0;
}

char *crypt(const char *key, const char *salt) {
    (void)key;
    (void)salt;
    return 0;
}

int tcsendbreak(int fd, int duration) {
    (void)fd;
    (void)duration;
    return 0;
}

/* Socket stubs - kernel has no networking */
int socket(int domain, int type, int protocol) {
    return do_syscall(SYSCALL_SOCKET, domain, type, protocol, 0);
}

int bind(int sockfd, const struct sockaddr *addr, unsigned int addrlen) {
    return do_syscall(SYSCALL_BIND, sockfd, (int)addr, addrlen, 0);
}

int connect(int sockfd, const struct sockaddr *addr, unsigned int addrlen) {
    return do_syscall(SYSCALL_CONNECT, sockfd, (int)addr, addrlen, 0);
}

int listen(int sockfd, int backlog) {
    return do_syscall(SYSCALL_LISTEN, sockfd, backlog, 0, 0);
}

int accept(int sockfd, struct sockaddr *addr, unsigned int *addrlen) {
    return do_syscall(SYSCALL_ACCEPT, sockfd, (int)addr, (int)addrlen, 0);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, unsigned int optlen) {
    return -1; // TODO
}

int shutdown(int sockfd, int how) {
    return -1; // TODO
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return do_syscall(SYSCALL_SEND, sockfd, (int)buf, len, flags);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return do_syscall(SYSCALL_RECV, sockfd, (int)buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, unsigned int addrlen) {
    (void)sockfd; (void)buf; (void)len; (void)flags; (void)dest_addr; (void)addrlen;
    return -1;
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, unsigned int *addrlen) {
    (void)sockfd; (void)buf; (void)len; (void)flags; (void)src_addr; (void)addrlen;
    return -1;
}

int getpeername(int sockfd, struct sockaddr *addr, unsigned int *addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    return -1;
}

int getsockname(int sockfd, struct sockaddr *addr, unsigned int *addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    return -1;
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    (void)sockfd; (void)msg; (void)flags;
    return -1;
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    (void)sockfd; (void)msg; (void)flags;
    return -1;
}

int getfb(void *info) {
    return do_syscall(SYSCALL_GETFBINFO, (int)info, 0, 0, 0);
}

int getmouse(void *state) {
    return do_syscall(SYSCALL_GETMOUSE, (int)state, 0, 0, 0);
}

int oom_kill(int pid) {
    return do_syscall(SYSCALL_OOM_KILL, pid, 0, 0, 0);
}

void *map_fb(void) {
    return (void *)(intptr_t)do_syscall(SYSCALL_MAP_FB, 0, 0, 0, 0);
}

int fb_set_active(int active) {
    return do_syscall(SYSCALL_FB_SET_ACTIVE, active, 0, 0, 0);
}
