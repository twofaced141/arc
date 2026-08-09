/* posix — smoke test for the new POSIX syscalls:
 *   lstat, pread, pwrite, fcntl, uname, sysinfo, getrlimit, setrlimit
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
static long bsd_open(const char *p, int flags, int mode) {
    return syscall3(BSD_SYS(SYS_OPEN), (long)p, flags, mode);
}
static long bsd_close(int fd) {
    return syscall1(BSD_SYS(SYS_CLOSE), fd);
}
static long bsd_lseek(int fd, long off, int whence) {
    return syscall3(BSD_SYS(SYS_LSEEK), fd, off, whence);
}
static long bsd_write_all(int fd, const void *buf, unsigned long cnt) {
    return syscall3(BSD_SYS(SYS_WRITE), fd, (long)buf, cnt);
}
static long bsd_read(int fd, void *buf, unsigned long cnt) {
    return syscall3(BSD_SYS(SYS_READ), fd, (long)buf, cnt);
}
static long bsd_symlink(const char *t, const char *p) {
    return syscall2(BSD_SYS(47 /* SYS_SYMLINK */), (long)t, (long)p);
}
static long bsd_unlink(const char *p) {
    return syscall1(BSD_SYS(25 /* SYS_UNLINK */), (long)p);
}
static long bsd_stat(const char *p, void *st) {
    return syscall2(BSD_SYS(23 /* SYS_STAT */), (long)p, (long)st);
}

struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65]; };
struct sysinfo {
    long uptime;
    unsigned long loads[3], totalram, freeram, sharedram, bufferram,
                  totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
};
struct stat {
    unsigned int st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid;
    unsigned long long st_size;
    unsigned int st_blksize, st_blocks, st_atime, st_mtime, st_ctime;
};
struct rlimit { unsigned long rlim_cur, rlim_max; };

#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_RDWR    2
#define O_NONBLOCK 0x800
#define S_IFMT   0170000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define RLIMIT_NOFILE 7
#define RLIMIT_STACK  3

#define CHECK(cond, msg) do { \
    if (!(cond)) { print("FAIL: "); print(msg); print("\n"); bsd_exit(1); } \
} while (0)

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

static void print_hex(unsigned long v) {
    char buf[20];
    int i = 20;
    buf[--i] = 'h', buf[--i] = 'x', buf[--i] = '0';
    do {
        int d = v % 16;
        buf[--i] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= 16;
    } while (v);
    bsd_write(1, buf + i, 20 - i);
}

__attribute__((noreturn))
void _start(void) {
    print("posix: start\n");

    /* ---- uname ---- */
    struct utsname u;
    long ur = uname(&u);
    if (ur != 0) {
        print("posix: uname returned "); print_dec(ur); print("\n");
        CHECK(ur == 0, "uname");
    }
    print("posix: uname sysname="); print(u.sysname);
    print(" release="); print(u.release);
    print(" machine="); print(u.machine); print("\n");

    /* ---- sysinfo ---- */
    struct sysinfo si;
    CHECK(sysinfo(&si) == 0, "sysinfo");
    print("posix: sysinfo uptime="); print_dec(si.uptime);
    print(" total="); print_hex(si.totalram);
    print(" free="); print_hex(si.freeram);
    print(" procs="); print_dec(si.procs); print("\n");
    CHECK(si.totalram > 0, "sysinfo totalram > 0");

    /* ---- pread / pwrite ---- */
    int fd = (int)bsd_open("/posix.dat", O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0, "open /posix.dat");
    CHECK(bsd_write_all(fd, "hello", 5) == 5, "write hello");
    CHECK(bsd_lseek(fd, 0, 1) == 5, "lseek SEEK_CUR after write == 5");
    CHECK(pwrite(fd, "world", 5, 5) == 5, "pwrite at 5");
    CHECK(bsd_lseek(fd, 0, 1) == 5, "pwrite did not move offset");

    char buf[16];
    CHECK(pread(fd, buf, 10, 0) == 10, "pread 10 at 0");
    buf[10] = 0;
    CHECK(buf[0] == 'h' && buf[4] == 'o' && buf[5] == 'w' && buf[9] == 'd',
          "pread contents helloworld");
    CHECK(bsd_lseek(fd, 0, 1) == 5, "pread did not move offset");

    /* ---- fcntl ---- */
    CHECK((fcntl(fd, 3 /*F_GETFL*/, 0) & 3) == O_RDWR, "F_GETFL accmode == O_RDWR");
    CHECK(fcntl(fd, 4 /*F_SETFL*/, O_NONBLOCK) == 0, "F_SETFL O_NONBLOCK");
    CHECK((fcntl(fd, 3 /*F_GETFL*/, 0) & O_NONBLOCK) != 0, "F_GETFL has O_NONBLOCK");
    CHECK(fcntl(fd, 1 /*F_GETFD*/, 0) == 0, "F_GETFD == 0");
    CHECK(fcntl(fd, 2 /*F_SETFD*/, 1 /*FD_CLOEXEC*/) == 0, "F_SETFD CLOEXEC");
    CHECK(fcntl(fd, 1 /*F_GETFD*/, 0) == 1, "F_GETFD == FD_CLOEXEC");
    CHECK(fcntl(fd, 2 /*F_SETFD*/, 0) == 0, "F_SETFD clear");
    long dupfd = fcntl(fd, 0 /*F_DUPFD*/, 10);
    CHECK(dupfd >= 10, "F_DUPFD >= 10");
    CHECK(bsd_close((int)dupfd) == 0, "close dupfd");

    /* ---- lstat vs stat ---- */
    bsd_unlink("/posix.lnk");
    CHECK(bsd_symlink("/posix.dat", "/posix.lnk") == 0, "symlink");
    struct stat st;
    CHECK(lstat("/posix.lnk", &st) == 0, "lstat");
    CHECK(S_ISLNK(st.st_mode), "lstat shows S_IFLNK");
    struct stat st2;
    CHECK(bsd_stat("/posix.lnk", &st2) == 0 && S_ISREG(st2.st_mode),
          "stat follows symlink");

    /* ---- getrlimit / setrlimit ---- */
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0, "getrlimit NOFILE");
    print("posix: rlimit NOFILE cur="); print_dec((long)rl.rlim_cur);
    print(" max="); print_dec((long)rl.rlim_max); print("\n");
    CHECK(rl.rlim_cur == 256, "NOFILE default 256");
    rl.rlim_cur = 100;
    CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0, "setrlimit NOFILE 100");
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0, "getrlimit again");
    CHECK(rl.rlim_cur == 100, "NOFILE now 100");

    bsd_close(fd);
    bsd_unlink("/posix.dat");
    bsd_unlink("/posix.lnk");

    print("posix: ALL OK\n");
    bsd_exit(0);
    __builtin_unreachable();
}
