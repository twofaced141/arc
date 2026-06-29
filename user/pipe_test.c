#include <stdint.h>

#define SYSCALL_GETPID  0
#define SYSCALL_WRITE   4
#define SYSCALL_READ    5
#define SYSCALL_CLOSE   9
#define SYSCALL_EXIT    3
#define SYSCALL_FORK    14
#define SYSCALL_WAITPID 16
#define SYSCALL_PIPE    22

static int syscall(int num, int a, int b, int c, int d) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory");
    return ret;
}

static void putc(char c) {
    syscall(SYSCALL_WRITE, 1, (int)(unsigned char *)&c, 1, 0);
}

static void puts(const char *s) {
    while (*s) putc(*s++);
}

static void putdec(int n) {
    if (n < 0) { putc('-'); n = -n; }
    char buf[12], *p = buf + 11;
    *p = '\0';
    do { *--p = '0' + (n % 10); n /= 10; } while (n);
    puts(p);
}

void _start(void) {
    int pid, status;
    int fds[2];
    char buf[64];
    int n;

    puts("pipe_test: starting\n");

    if (syscall(SYSCALL_PIPE, (int)fds, 0, 0, 0) < 0) {
        puts("pipe_test: pipe failed\n");
        syscall(SYSCALL_EXIT, 1, 0, 0, 0);
    }

    putdec(fds[0]); puts("=read_fd ");
    putdec(fds[1]); puts("=write_fd\n");

    pid = syscall(SYSCALL_FORK, 0, 0, 0, 0);
    if (pid < 0) {
        puts("pipe_test: fork failed\n");
        syscall(SYSCALL_EXIT, 1, 0, 0, 0);
    }

    if (pid == 0) {
        /* child: read from pipe */
        syscall(SYSCALL_CLOSE, fds[1], 0, 0, 0);
        puts("child: reading...\n");
        n = syscall(SYSCALL_READ, fds[0], (int)buf, 63, 0);
        if (n > 0) {
            buf[n] = '\0';
            puts("child: got '");
            puts(buf);
            puts("'\n");
        } else {
            puts("child: read returned ");
            putdec(n);
            putc('\n');
        }
        syscall(SYSCALL_CLOSE, fds[0], 0, 0, 0);
        syscall(SYSCALL_EXIT, 42, 0, 0, 0);
    }

    /* parent: write to pipe */
    syscall(SYSCALL_CLOSE, fds[0], 0, 0, 0);
    puts("parent: writing...\n");
    syscall(SYSCALL_WRITE, fds[1], (int)"Hello from parent!", 19, 0);
    syscall(SYSCALL_CLOSE, fds[1], 0, 0, 0);

    status = 0;
    syscall(SYSCALL_WAITPID, pid, (int)&status, 0, 0);
    puts("parent: child exited with status ");
    putdec(status);
    putc('\n');

    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
