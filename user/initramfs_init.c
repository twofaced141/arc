#include <stddef.h>
#include <stdint.h>

static int sys_getpid(void) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(0) : "memory");
    return ret;
}

static int sys_open(const char *path, int flags) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(8), "b"((uint32_t)path), "c"((uint32_t)flags) : "memory");
    return ret;
}

static int sys_read(int fd, void *buf, int count) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(5), "b"((uint32_t)fd), "c"((uint32_t)buf), "d"((uint32_t)count) : "memory");
    return ret;
}

static int sys_write(int fd, const void *buf, int count) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(4), "b"((uint32_t)fd), "c"((uint32_t)buf), "d"((uint32_t)count) : "memory");
    return ret;
}

static int sys_close(int fd) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(9), "b"((uint32_t)fd) : "memory");
    return ret;
}

static int sys_fork(void) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(14) : "memory");
    return ret;
}

static int sys_execve(const char *path, const char *arg) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(15), "b"((uint32_t)path), "c"((uint32_t)arg) : "memory");
    return ret;
}

static int sys_waitpid(int pid, int *status, int flags) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(16), "b"((uint32_t)pid), "c"((uint32_t)status), "d"((uint32_t)flags) : "memory");
    return ret;
}

static int sys_chdir(const char *path) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(17), "b"((uint32_t)path) : "memory");
    return ret;
}

static int sys_stat(const char *path, void *buf) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(29), "b"((uint32_t)path), "c"((uint32_t)buf) : "memory");
    return ret;
}

static int sys_mount(const char *dev, const char *target) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "0"(53), "b"((uint32_t)dev), "c"((uint32_t)target) : "memory");
    return ret;
}

static void print(const char *s) {
    while (*s) {
        sys_write(1, s, 1);
        s++;
    }
}

/* Entry point: try /bin/init, fall back to /bin/shell */
void _start(void) {
    /* Use NULL args to avoid strncpy_from_user failure */
    int r = sys_execve("/bin/init", 0);
    if (r < 0)
        r = sys_execve("/bin/shell", 0);

    /* Last resort: loop printing dots so we can see if we're alive */
    for (;;) {
        sys_write(1, ".", 1);
        for (volatile int i = 0; i < 10000000; i++);
    }
}
