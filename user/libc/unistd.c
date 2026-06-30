#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

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
#define SYSCALL_GETEUID  43
#define SYSCALL_GETEGID  44

static int syscall(int num, int a, int b, int c, int d) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory");
    return ret;
}

int read(int fd, void *buf, size_t count) {
    return syscall(SYSCALL_READ, fd, (int)buf, (int)count, 0);
}

int write(int fd, const void *buf, size_t count) {
    return syscall(SYSCALL_WRITE, fd, (int)buf, (int)count, 0);
}

int open(const char *path, int flags, ...) {
    return syscall(SYSCALL_OPEN, (int)path, flags, 0, 0);
}

int close(int fd) {
    return syscall(SYSCALL_CLOSE, fd, 0, 0, 0);
}

pid_t fork(void) {
    return syscall(SYSCALL_FORK, 0, 0, 0, 0);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    (void)envp;
    if (!argv || !argv[0]) {
        return syscall(SYSCALL_EXECVE, (int)path, 0, 0, 0);
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
    return syscall(SYSCALL_EXECVE, (int)path, (int)flat, 0, 0);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    int st;
    int r = syscall(SYSCALL_WAITPID, pid, (int)&st, options, 0);
    if (status) *status = st;
    return r;
}

unsigned int sleep(unsigned int seconds) {
    syscall(SYSCALL_SLEEP, (int)(seconds * 1000), 0, 0, 0);
    return 0;
}

int brk(void *addr) {
    return syscall(SYSCALL_BRK, (int)addr, 0, 0, 0);
}

void *sbrk(intptr_t increment) {
    return (void *)(intptr_t)syscall(SYSCALL_SBRK, (int)increment, 0, 0, 0);
}

pid_t getpid(void) {
    return syscall(SYSCALL_GETPID, 0, 0, 0, 0);
}

char *getcwd(char *buf, size_t size) {
    int n = syscall(SYSCALL_GETCWD, (int)buf, (int)size, 0, 0);
    if (n < 0) return NULL;
    return buf;
}

int chdir(const char *path) {
    return syscall(SYSCALL_CHDIR, (int)path, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    return syscall(SYSCALL_LSEEK, fd, offset, whence, 0);
}

int fstat(int fd, struct stat *buf) {
    return syscall(SYSCALL_FSTAT, fd, (int)buf, 0, 0);
}

int dup2(int oldfd, int newfd) {
    return syscall(SYSCALL_DUP2, oldfd, newfd, 0, 0);
}

int pipe(int pipefd[2]) {
    return syscall(SYSCALL_PIPE, (int)pipefd, 0, 0, 0);
}

int ioctl(int fd, int request, void *arg) {
    return syscall(SYSCALL_IOCTL, fd, request, (int)arg, 0);
}

int kill(pid_t pid, int sig) {
    return syscall(SYSCALL_KILL, pid, sig, 0, 0);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)fd; (void)offset;
    void *ret = (void *)(intptr_t)syscall(SYSCALL_MMAP, (int)addr, (int)length, prot, flags);
    if ((intptr_t)ret == -1) return (void *)-1;
    return ret;
}

int munmap(void *addr, size_t length) {
    return syscall(SYSCALL_MUNMAP, (int)addr, (int)length, 0, 0);
}

int uname(void *buf) {
    return syscall(SYSCALL_UNAME, (int)buf, 0, 0, 0);
}

int yield(void) {
    return syscall(SYSCALL_YIELD, 0, 0, 0, 0);
}

int getticks(void) {
    return syscall(SYSCALL_GETTICKS, 0, 0, 0, 0);
}

int gettime(struct rtc_time *t) {
    return syscall(SYSCALL_GETTIME, (int)t, 0, 0, 0);
}

int stat(const char *path, struct stat *buf) {
    return syscall(SYSCALL_STAT, (int)path, (int)buf, 0, 0);
}

int lstat(const char *path, struct stat *buf) {
    return syscall(SYSCALL_LSTAT, (int)path, (int)buf, 0, 0);
}

int getdents(const char *path, void *dirp, unsigned int count) {
    return syscall(SYSCALL_GETDENTS, (int)path, (int)dirp, (int)count, 0);
}

int unlink(const char *path) {
    return syscall(SYSCALL_UNLINK, (int)path, 0, 0, 0);
}

int rename(const char *oldpath, const char *newpath) {
    return syscall(SYSCALL_RENAME, (int)oldpath, (int)newpath, 0, 0);
}

int mkdir(const char *path, unsigned int mode) {
    return syscall(SYSCALL_MKDIR, (int)path, (int)mode, 0, 0);
}

int rmdir(const char *path) {
    return syscall(SYSCALL_RMDIR, (int)path, 0, 0, 0);
}

int chmod(const char *path, unsigned int mode) {
    return syscall(SYSCALL_CHMOD, (int)path, (int)mode, 0, 0);
}

int chown(const char *path, unsigned int owner, unsigned int group) {
    return syscall(SYSCALL_CHOWN, (int)path, (int)owner, (int)group, 0);
}

int access(const char *path, int mode) {
    return syscall(SYSCALL_ACCESS, (int)path, (int)mode, 0, 0);
}

unsigned int getuid(void) {
    return (unsigned int)syscall(SYSCALL_GETUID, 0, 0, 0, 0);
}

unsigned int getgid(void) {
    return (unsigned int)syscall(SYSCALL_GETGID, 0, 0, 0, 0);
}

unsigned int geteuid(void) {
    return (unsigned int)syscall(SYSCALL_GETEUID, 0, 0, 0, 0);
}

unsigned int getegid(void) {
    return (unsigned int)syscall(SYSCALL_GETEGID, 0, 0, 0, 0);
}
