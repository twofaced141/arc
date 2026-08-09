/* sigexec — exec-time signal state probe.
 *
 * Kills itself with SIGUSR1 and exits.  The parent (init) sets up the
 * signal disposition BEFORE exec and decides from the exit status
 * whether exec reset the state correctly:
 *   - caught handler must revert to SIG_DFL  → dies by SIGUSR1
 *   - SIG_IGN must survive                  → survives, exits 0
 */

#include "syscall.h"

static unsigned long my_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned long)(p - s);
}

static long bsd_write(int fd, const void *buf, unsigned long cnt) {
    return syscall3(BSD_SYS(SYS_WRITE), fd, (long)buf, cnt);
}
static long bsd_exit(long code) {
    return syscall1(BSD_SYS(SYS_EXIT), code);
}
static long bsd_getpid(void) {
    return syscall0(BSD_SYS(SYS_GETPID));
}
static long bsd_kill(long pid, int sig) {
    return syscall2(BSD_SYS(SYS_KILL), pid, sig);
}

static void print(const char *s) {
    bsd_write(1, s, my_strlen(s));
}

static void print_dec(long v) {
    char buf[20];
    int i = 20;
    if (v < 0) {
        bsd_write(1, "-", 1);
        v = -v;
    }
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);
    bsd_write(1, buf + i, 20 - i);
}

__attribute__((noreturn))
void _start(void) {
    print("sigexec: pid=");
    print_dec(bsd_getpid());
    print(" killing self with SIGUSR1\n");

    bsd_kill(bsd_getpid(), 10 /* SIGUSR1 */);
    bsd_exit(0);
    __builtin_unreachable();
}
