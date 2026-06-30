#include <stdint.h>

#define SYSCALL_FORK    14
#define SYSCALL_EXECVE  15
#define SYSCALL_WAITPID 16
#define SYSCALL_EXIT    3

static int syscall(int num, int a, int b, int c, int d) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory");
    return ret;
}

void _start(void) {
    for (;;) {
        int pid = syscall(SYSCALL_FORK, 0, 0, 0, 0);
        if (pid < 0)
            continue;
        if (pid == 0) {
            syscall(SYSCALL_EXECVE, (int)"/bin/shell", 0, 0, 0);
            syscall(SYSCALL_EXIT, 1, 0, 0, 0);
        }
        syscall(SYSCALL_WAITPID, pid, 0, 0, 0);
    }
}
