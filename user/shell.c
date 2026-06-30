#include <stdint.h>

#define SYSCALL_PUTC    1
#define SYSCALL_YIELD   2
#define SYSCALL_EXIT    3
#define SYSCALL_WRITE   4
#define SYSCALL_READ    5
#define SYSCALL_OPEN    8
#define SYSCALL_CLOSE   9
#define SYSCALL_FORK    14
#define SYSCALL_EXECVE  15
#define SYSCALL_WAITPID 16
#define SYSCALL_CHDIR   17

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
    syscall(SYSCALL_PUTC, c, 0, 0, 0);
}

static void puts(const char *s) {
    while (*s) putc(*s++);
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static char read_char(void) {
    char c;
    while (syscall(SYSCALL_READ, 0, (int)&c, 1, 0) <= 0)
        syscall(SYSCALL_YIELD, 0, 0, 0, 0);
    return c;
}

void _start(void) {
    char input[256];
    char cmd[64];
    char path[64];

    for (;;) {
        puts("$ ");

        int pos = 0;
        for (;;) {
            char c = read_char();
            if (c == '\n') {
                putc('\n');
                break;
            }
            if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    putc('\b');
                    putc(' ');
                    putc('\b');
                }
                continue;
            }
            if (pos < 254) {
                putc(c);
                input[pos++] = c;
            }
        }
        input[pos] = '\0';

        const char *p = input;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        const char *cmd_start = p;
        while (*p && *p != ' ' && *p != '\t') p++;

        int cmd_len = p - cmd_start;
        if (cmd_len > 63) cmd_len = 63;
        int i;
        for (i = 0; i < cmd_len; i++) cmd[i] = cmd_start[i];
        cmd[i] = '\0';

        while (*p == ' ' || *p == '\t') p++;
        const char *args_start = p;

        if (streq(cmd, "exit")) {
            syscall(SYSCALL_EXIT, 0, 0, 0, 0);
        }

        if (streq(cmd, "cd")) {
            if (*args_start) {
                if (syscall(SYSCALL_CHDIR, (int)args_start, 0, 0, 0) < 0) {
                    puts("cd: ");
                    puts(args_start);
                    puts(": no such directory\n");
                }
            }
            continue;
        }

        int has_slash = 0;
        for (i = 0; cmd[i]; i++)
            if (cmd[i] == '/') { has_slash = 1; break; }

        if (has_slash) {
            i = 0;
            while (cmd[i] && i < 63) { path[i] = cmd[i]; i++; }
            path[i] = '\0';
        } else {
            char *prefix = "/bin/";
            int pi = 0;
            while (*prefix) path[pi++] = *prefix++;
            i = 0;
            while (cmd[i] && pi < 63) path[pi++] = cmd[i++];
            path[pi] = '\0';
        }

        int fd = syscall(SYSCALL_OPEN, (int)path, 0, 0, 0);
        if (fd < 0) {
            puts(cmd);
            puts(": not found\n");
            continue;
        }
        syscall(SYSCALL_CLOSE, fd, 0, 0, 0);

        int pid = syscall(SYSCALL_FORK, 0, 0, 0, 0);
        if (pid < 0) continue;
        if (pid == 0) {
            syscall(SYSCALL_EXECVE, (int)path, (int)args_start, 0, 0);
            syscall(SYSCALL_EXIT, 1, 0, 0, 0);
        }
        syscall(SYSCALL_WAITPID, pid, 0, 0, 0);
    }
}
