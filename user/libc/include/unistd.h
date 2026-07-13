#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int open(const char *path, int flags, ...);
int close(int fd);
pid_t fork(void);
pid_t vfork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *path, const char *arg, ...);

void _exit(int status) __attribute__((noreturn));
pid_t waitpid(pid_t pid, int *status, int options);
unsigned int sleep(unsigned int seconds);
int brk(void *addr);
void *sbrk(intptr_t increment);
pid_t getpid(void);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int chroot(const char *path);
off_t lseek(int fd, off_t offset, int whence);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);
int ioctl(int fd, int request, ...);
int kill(pid_t pid, int sig);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mlockall(int flags);
int yield(void);
int getticks(void);
int getdents(const char *path, void *dirp, unsigned int count);
int unlink(const char *path);
int rename(const char *oldpath, const char *newpath);
int mkdir(const char *path, unsigned int mode);
int mknod(const char *path, unsigned int mode, unsigned int dev);
int mkdirat(int dirfd, const char *pathname, unsigned int mode);
int rmdir(const char *path);
int chmod(const char *path, unsigned int mode);
int chown(const char *path, unsigned int owner, unsigned int group);
int access(const char *path, int mode);
unsigned int getuid(void);
unsigned int getgid(void);
unsigned int geteuid(void);
unsigned int getegid(void);
int sigprocmask(int how, const uint32_t *set, uint32_t *oldset);
int sigpending(uint32_t *set);
int sigsuspend(const uint32_t *mask);
int readlink(const char *path, char *buf, size_t bufsiz);
int readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
int openat(int dirfd, const char *pathname, int flags, ...);
int link(const char *oldpath, const char *newpath);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int posix_fallocate(int fd, off_t offset, off_t len);
unsigned int alarm(unsigned int seconds);
int symlink(const char *target, const char *linkpath);
int fchdir(int fd);
int fchmod(int fd, unsigned int mode);
int fchown(int fd, unsigned int owner, unsigned int group);
int getgroups(int size, unsigned int *list);
int setgroups(size_t size, const unsigned int *list);
int dup(int oldfd);
int setuid(unsigned int uid);
int setgid(unsigned int gid);
int seteuid(unsigned int euid);
int setegid(unsigned int egid);
pid_t getppid(void);
int pause(void);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);
int nice(int inc);
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);
int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags);
unsigned int umask(unsigned int mask);
int reboot(int cmd);
int poweroff(void);
ssize_t readahead(int fd, off64_t offset, size_t count);
int sethostname(const char *name, size_t len);
int gethostname(char *name, size_t len);
char *crypt(const char *key, const char *salt);
int setpgid(int pid, int pgid);
int getpgid(int pid);
pid_t setsid(void);
pid_t getsid(pid_t pid);
int tcsetpgrp(int fd, int pgrp);
int tcgetpgrp(int fd);

struct rtc_time {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};
int gettime(struct rtc_time *t);

int getpagesize(void);
int isatty(int fd);
char *ttyname(int fd);
long syscall(long number, ...);
extern int optind, opterr, optopt;
extern char *optarg;
long sysconf(int name);
long pathconf(const char *path, int name);
int unlinkat(int dirfd, const char *pathname, int flags);
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
int faccessat(int dirfd, const char *pathname, int mode, int flags);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int symlinkat(const char *target, int newdirfd, const char *linkpath);
int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev);
int lchown(const char *pathname, uid_t owner, gid_t group);
int futimens(int fd, const struct timespec times[2]);

#define _SC_ARG_MAX         0
#define _SC_PAGESIZE        1
#define _SC_PAGE_SIZE       _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN 2

#define _PC_NAME_MAX 1
#define _PC_ASYNC_IO       2
#define _PC_CHOWN_RESTRICTED 3
#define _PC_FILESIZEBITS   4
#define _PC_LINK_MAX       5
#define _PC_MAX_CANON      6
#define _PC_MAX_INPUT      7
#define _PC_NO_TRUNC       8
#define _PC_PATH_MAX       9
#define _PC_PIPE_BUF       10
#define _PC_PRIO_IO        11
#define _PC_SYMLINK_MAX    12
#define _PC_SYNC_IO        13
#define _PC_VDISABLE       14

#define _CS_PATH           1
#define _CS_V7_ENV         2

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000
#define _SC_OPEN_MAX        3
#define _SC_CLK_TCK         4
#define _SC_HOST_NAME_MAX   5
#define _SC_PIPE_BUF        6
#define _SC_PHYS_PAGES      7
#define _SC_AVPHYS_PAGES   8
#define _SC_AIO_LISTIO_MAX  9
#define _SC_AIO_MAX         10
#define _SC_AIO_PRIO_DELTA_MAX 11
#define _SC_ATEXIT_MAX      12
#define _SC_BC_BASE_MAX     13
#define _SC_BC_DIM_MAX      14
#define _SC_BC_SCALE_MAX    15
#define _SC_BC_STRING_MAX   16
#define _SC_CHILD_MAX       17
#define _SC_COLL_WEIGHTS_MAX 18
#define _SC_DELAYTIMER_MAX  19
#define _SC_EXPR_NEST_MAX   20
#define _SC_IOV_MAX         21
#define _SC_LINE_MAX        22
#define _SC_LOGIN_NAME_MAX  23
#define _SC_NGROUPS_MAX     24
#define _SC_MQ_OPEN_MAX     25
#define _SC_MQ_PRIO_MAX     26
#define _SC_NPROCESSORS_CONF 27
#define _SC_RAW_SOCKETS     28
#define _SC_RE_DUP_MAX      29
#define _SC_RTSIG_MAX       30
#define _SC_SEM_NSEMS_MAX   31
#define _SC_SEM_VALUE_MAX   32
#define _SC_SIGQUEUE_MAX    33
#define _SC_STREAM_MAX      34
#define _SC_SYMLOOP_MAX     35
#define _SC_TIMER_MAX       36
#define _SC_TTY_NAME_MAX    37
#define _SC_TZNAME_MAX      38
#define _SC_UIO_MAXIOV      39
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 40
#define _SC_THREAD_KEYS_MAX 41
#define _SC_THREAD_STACK_MIN 42
#define _SC_THREAD_THREADS_MAX 43

#define _SC_2_C_BIND       100
#define _SC_2_C_DEV        101
#define _SC_2_CHAR_TERM    102
#define _SC_2_FORT_DEV     103
#define _SC_2_FORT_RUN     104
#define _SC_2_LOCALEDEF    105
#define _SC_2_PBS          106
#define _SC_2_PBS_ACCOUNTING 107
#define _SC_2_PBS_CHECKPOINT 108
#define _SC_2_PBS_LOCATE   109
#define _SC_2_PBS_MESSAGE  110
#define _SC_2_PBS_TRACK    111
#define _SC_2_SW_DEV       112
#define _SC_2_UPE          113
#define _SC_2_VERSION      114

#define _SC_XOPEN_CRYPT    200
#define _SC_XOPEN_ENH_I18N 201
#define _SC_XOPEN_REALTIME 202
#define _SC_XOPEN_REALTIME_THREADS 203
#define _SC_XOPEN_SHM      204
#define _SC_XOPEN_STREAMS  205
#define _SC_XOPEN_UNIX     206
#define _SC_XOPEN_UUCP     207
#define _SC_XOPEN_VERSION  208

#define _SC_ADVISORY_INFO  209
#define _SC_BARRIERS       210
#define _SC_ASYNCHRONOUS_IO 211
#define _SC_CLOCK_SELECTION 212
#define _SC_CPUTIME        213
#define _SC_FSYNC          214
#define _SC_IPV6           215
#define _SC_JOB_CONTROL    216
#define _SC_MAPPED_FILES   217
#define _SC_MEMLOCK        218
#define _SC_MEMLOCK_RANGE  219
#define _SC_MEMORY_PROTECTION 220
#define _SC_MESSAGE_PASSING 221
#define _SC_MONOTONIC_CLOCK 222
#define _SC_PRIORITY_SCHEDULING 223
#define _SC_READER_WRITER_LOCKS 224
#define _SC_REALTIME_SIGNALS 225
#define _SC_REGEXP         226
#define _SC_SAVED_IDS      227
#define _SC_SEMAPHORES     228
#define _SC_SHARED_MEMORY_OBJECTS 229
#define _SC_SHELL          230
#define _SC_SPAWN          231
#define _SC_SPIN_LOCKS     232
#define _SC_SPORADIC_SERVER 233
#define _SC_SS_REPL_MAX    234
#define _SC_SYNCHRONIZED_IO 235
#define _SC_THREAD_ATTR_STACKADDR 236
#define _SC_THREAD_ATTR_STACKSIZE 237
#define _SC_THREAD_CPUTIME 238
#define _SC_THREAD_PRIO_INHERIT 239
#define _SC_THREAD_PRIO_PROTECT 240
#define _SC_THREAD_PRIORITY_SCHEDULING 241
#define _SC_THREAD_PROCESS_SHARED 242
#define _SC_THREAD_ROBUST_PRIO_INHERIT 243
#define _SC_THREAD_ROBUST_PRIO_PROTECT 244
#define _SC_THREAD_SAFE_FUNCTIONS 245
#define _SC_THREAD_SPORADIC_SERVER 246
#define _SC_THREADS        247
#define _SC_TIMEOUTS       248
#define _SC_TIMERS         249
#define _SC_TRACE          250
#define _SC_TRACE_EVENT_FILTER 251
#define _SC_TRACE_EVENT_NAME_MAX 252
#define _SC_TRACE_INHERIT  253
#define _SC_TRACE_LOG      254
#define _SC_TRACE_NAME_MAX 255
#define _SC_TRACE_SYS_MAX  256
#define _SC_TRACE_USER_EVENT_MAX 257
#define _SC_TYPED_MEMORY_OBJECTS 258
#define _SC_V7_ILP32_OFF32 259
#define _SC_V7_ILP32_OFFBIG 260
#define _SC_V7_LP64_OFF64  261
#define _SC_V7_LPBIG_OFFBIG 262
#define _SC_VERSION       263

#define _POSIX2_BC_BASE_MAX         99
#define _POSIX2_BC_DIM_MAX          2048
#define _POSIX2_BC_SCALE_MAX        99
#define _POSIX2_BC_STRING_MAX       1000
#define _POSIX2_CHARCLASS_NAME_MAX  14
#define _POSIX2_COLL_WEIGHTS_MAX    2
#define _POSIX2_EXPR_NEST_MAX       32
#define _POSIX2_LINE_MAX            2048
#define _POSIX2_RE_DUP_MAX          255

#define _POSIX_AIO_LISTIO_MAX      2
#define _POSIX_AIO_MAX             1
#define _POSIX_ARG_MAX             4096
#define _POSIX_CHILD_MAX           25
#define _POSIX_DELAYTIMER_MAX      32
#define _POSIX_HOST_NAME_MAX       255
#define _POSIX_LINK_MAX            8
#define _POSIX_LOGIN_NAME_MAX      9
#define _POSIX_MAX_CANON           255
#define _POSIX_MAX_INPUT           255
#define _POSIX_NAME_MAX            14
#define _POSIX_NGROUPS_MAX         0
#define _POSIX_OPEN_MAX            16
#define _POSIX_PATH_MAX            256
#define _POSIX_PIPE_BUF            512
#define _POSIX_RE_DUP_MAX          255
#define _POSIX_RTSIG_MAX           8
#define _POSIX_SEM_NSEMS_MAX       256
#define _POSIX_SEM_VALUE_MAX       32767
#define _POSIX_SIGQUEUE_MAX        32
#define _POSIX_SSIZE_MAX           INT_MAX
#define _POSIX_STREAM_MAX          8
#define _POSIX_SYMLINK_MAX         255
#define _POSIX_SYMLOOP_MAX         8
#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX     128
#define _POSIX_THREAD_THREADS_MAX  256
#define _POSIX_TIMER_MAX           32
#define _POSIX_TTY_NAME_MAX        9
#define _POSIX_TZNAME_MAX          3

size_t confstr(int name, char *buf, size_t len);

int getfb(void *info);
int getmouse(void *state);
int oom_kill(int pid);
void *map_fb(void);
int fb_set_active(int active);

typedef struct {
    uint32_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t type;
} fb_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
    int8_t  wheel;
    uint8_t present;
} mouse_state_t;

#endif
