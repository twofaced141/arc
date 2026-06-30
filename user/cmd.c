#include <stdint.h>
#include <stddef.h>

#define SYSCALL_GETPID  0
#define SYSCALL_PUTC    1
#define SYSCALL_EXIT    3
#define SYSCALL_WRITE   4
#define SYSCALL_READ    5
#define SYSCALL_OPEN    8
#define SYSCALL_CLOSE   9
#define SYSCALL_GETCWD  18
#define SYSCALL_LISTDIR 19
#define SYSCALL_KILL    20
#define SYSCALL_GETTIME 24
#define SYSCALL_KMALLOC_TEST 27

static __attribute__((unused)) int syscall(int num, int a, int b, int c, int d) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory");
    return ret;
}

static __attribute__((unused)) void putc(char c) {
    syscall(SYSCALL_PUTC, c, 0, 0, 0);
}

static __attribute__((unused)) void puts(const char *s) {
    while (*s) putc(*s++);
}

static __attribute__((unused)) void putdec(uint32_t n) {
    char buf[12], *p = buf + 11;
    *p = '\0';
    do { *--p = '0' + (n % 10); n /= 10; } while (n);
    puts(p);
}

#ifdef CMD_PWD
void _start(void) {
    char buf[256];
    int n = syscall(SYSCALL_GETCWD, (int)buf, 256, 0, 0);
    if (n >= 0) {
        syscall(SYSCALL_WRITE, 1, (int)buf, n, 0);
        putc('\n');
    }
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_GETPID
void _start(void) {
    int pid = syscall(SYSCALL_GETPID, 0, 0, 0, 0);
    putdec(pid);
    putc('\n');
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_CLEAR
void _start(void) {
    for (int i = 0; i < 25; i++)
    putc('\n');
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_DATE
struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

static const char digits[] = "0123456789";

static void put2(uint8_t n) {
    putc(digits[n / 10]);
    putc(digits[n % 10]);
}

void _start(void) {
    struct rtc_time t;
    int r = syscall(SYSCALL_GETTIME, (int)&t, 0, 0, 0);
    if (r < 0) {
        puts("date: failed\n");
        syscall(SYSCALL_EXIT, 1, 0, 0, 0);
    }
    putdec(t.year);
    putc('-');
    put2(t.month);
    putc('-');
    put2(t.day);
    putc(' ');
    put2(t.hour);
    putc(':');
    put2(t.minute);
    putc(':');
    put2(t.second);
    putc('\n');
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_LS
void _start(void) {
    char buf[1024];
    const char *args = (const char *)0xBFFFF000;
    int n = syscall(SYSCALL_LISTDIR, (int)(*args ? args : 0), (int)buf, 1024, 0);
    if (n >= 0)
        syscall(SYSCALL_WRITE, 1, (int)buf, n, 0);
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_KILL
void _start(void) {
    const char *args = (const char *)0xBFFFF000;
    if (*args) {
        uint32_t pid = 0;
        while (*args >= '0' && *args <= '9')
            pid = pid * 10 + (*args++ - '0');
        syscall(SYSCALL_KILL, (int)pid, 0, 0, 0);
    }
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_CAT
void _start(void) {
    const char *args = (const char *)0xBFFFF000;
    if (!*args) {
        puts("cat: no file\n");
        syscall(SYSCALL_EXIT, 1, 0, 0, 0);
    }
    int fd = syscall(SYSCALL_OPEN, (int)args, 0, 0, 0);
    if (fd < 0) {
        puts("cat: ");
        puts(args);
        puts(": not found\n");
        syscall(SYSCALL_EXIT, 1, 0, 0, 0);
    }
    char buf[1024];
    int n;
    while ((n = syscall(SYSCALL_READ, fd, (int)buf, 1024, 0)) > 0)
        syscall(SYSCALL_WRITE, 1, (int)buf, n, 0);
    syscall(SYSCALL_CLOSE, fd, 0, 0, 0);
    putc('\n');
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif

#ifdef CMD_KMALLOC_TEST
static void putKB(uint32_t bytes) {
    if (bytes >= 1024 * 1024) {
        putdec(bytes / (1024 * 1024));
        puts(" MB");
    } else if (bytes >= 1024) {
        putdec(bytes / 1024);
        puts(" KB");
    } else {
        putdec(bytes);
        puts(" B");
    }
}

void _start(void) {
    const char *args = (const char *)0xBFFFF000;
    if (*args == 's' || *args == '\0') {
        uint32_t size = 64;
        int r = syscall(SYSCALL_KMALLOC_TEST, 0, (int)size, 0, 0);
        if (r < 0) {
            puts("kmalloc_test: alloc failed\n");
        } else {
            putKB(size);
            puts(" alloc/fill/verify/free OK\n");
        }
    }
    if (*args == 'o' || *args == '\0') {
        int max = syscall(SYSCALL_KMALLOC_TEST, 1, 0, 0, 0);
        if (max < 0) {
            puts("kmalloc_test: OOM test failed\n");
        } else {
            puts("largest single kmalloc: ");
            putKB((uint32_t)max);
            putc('\n');
        }
    }
    syscall(SYSCALL_EXIT, 0, 0, 0, 0);
}
#endif
