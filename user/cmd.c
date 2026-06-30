#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

#ifdef CMD_PWD
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[256];
    if (getcwd(buf, sizeof(buf)))
        printf("%s\n", buf);
    return 0;
}
#endif

#ifdef CMD_GETPID
int main(void) {
    printf("%d\n", getpid());
    return 0;
}
#endif

#ifdef CMD_CLEAR
int main(void) {
    for (int i = 0; i < 25; i++)
        putchar('\n');
    return 0;
}
#endif

#ifdef CMD_DATE
int main(void) {
    struct rtc_time t;
    if (gettime(&t) < 0) {
        printf("date: failed\n");
        return 1;
    }
    printf("%u-%02u-%02u %02u:%02u:%02u\n",
           t.year, t.month, t.day,
           t.hour, t.minute, t.second);
    return 0;
}
#endif

#ifdef CMD_LS
int main(int argc, char **argv) {
    char buf[1024];
    const char *dir = (argc > 1 && argv[1][0]) ? argv[1] : NULL;
    int n = 0;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(n)
            : "a"(19), "b"((int)dir), "c"((int)buf), "d"(sizeof(buf)), "S"(0)
        : "memory");
    if (n > 0)
        write(1, buf, n);
    return 0;
}
#endif

#ifdef CMD_KILL
int main(int argc, char **argv) {
    if (argc > 1) {
        int pid = atoi(argv[1]);
        kill(pid, 0);
    }
    return 0;
}
#endif

#ifdef CMD_CAT
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("cat: no file\n");
        return 1;
    }
    int fd = open(argv[1], 0);
    if (fd < 0) {
        printf("cat: %s: not found\n", argv[1]);
        return 1;
    }
    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    putchar('\n');
    return 0;
}
#endif

#ifdef CMD_UNAME
#define UTSNAME_LEN 65
typedef struct {
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
} utsname_t;

int main(int argc, char **argv) {
    int flags = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        while (*a) {
            if (*a == 'a') flags = 0x3F;
            if (*a == 's') flags |= 2;
            if (*a == 'n') flags |= 4;
            if (*a == 'r') flags |= 8;
            if (*a == 'v') flags |= 16;
            if (*a == 'm') flags |= 32;
            a++;
        }
    }
    if (flags == 0) flags = 2 | 4 | 8 | 16 | 32;

    utsname_t uts;
    uname(&uts);

    if (flags & 2) { printf("%s ", uts.sysname); }
    if (flags & 4) { printf("%s ", uts.nodename); }
    if (flags & 8) { printf("%s ", uts.release); }
    if (flags & 16) { printf("%s ", uts.version); }
    if (flags & 32) { printf("%s ", uts.machine); }
    putchar('\n');
    return 0;
}
#endif

#ifdef CMD_SLEEP
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("sleep: missing operand\n");
        return 1;
    }
    unsigned int secs = (unsigned int)atoi(argv[1]);
    if (secs > 0) sleep(secs);
    return 0;
}
#endif

#ifdef CMD_ECHO
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        printf("%s", argv[i]);
    }
    putchar('\n');
    return 0;
}
#endif

#ifdef CMD_TOUCH
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("touch: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT);
        if (fd < 0) {
            printf("touch: %s: cannot create\n", argv[i]);
            status = 1;
        } else {
            close(fd);
        }
    }
    return status;
}
#endif

#ifdef CMD_CP
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("cp: missing operand\n");
        return 1;
    }
    int fd_src = open(argv[1], 0);
    if (fd_src < 0) {
        printf("cp: %s: no such file\n", argv[1]);
        return 1;
    }
    struct stat st;
    if (stat(argv[1], &st) < 0) {
        printf("cp: %s: stat failed\n", argv[1]);
        close(fd_src);
        return 1;
    }
    int fd_dst = open(argv[2], O_CREAT);
    if (fd_dst < 0) {
        printf("cp: %s: cannot create\n", argv[2]);
        close(fd_src);
        return 1;
    }
    uint32_t remaining = st.st_size;
    uint32_t offset = 0;
    while (remaining > 0) {
        char buf[512];
        uint32_t chunk = remaining < 512 ? remaining : 512;
        int n = read(fd_src, buf, chunk);
        if (n <= 0) break;
        if (write(fd_dst, buf, n) != n) {
            printf("cp: %s: write error\n", argv[2]);
            break;
        }
        offset += n;
        remaining -= n;
    }
    close(fd_src);
    close(fd_dst);
    return 0;
}
#endif

#ifdef CMD_MV
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("mv: missing operand\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        printf("mv: cannot rename '%s' to '%s'\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
#endif

#ifdef CMD_PS
int main(void) {
    struct dirent dirs[16];
    int n = getdents("/proc", dirs, 16);
    if (n < 0) {
        printf("ps: cannot read /proc\n");
        return 1;
    }
    printf("%-6s %-10s %s\n", "PID", "STATE", "NAME");
    for (int i = 0; i < n; i++) {
        int is_pid = 1;
        for (int j = 0; dirs[i].d_name[j]; j++) {
            if (dirs[i].d_name[j] < '0' || dirs[i].d_name[j] > '9') {
                is_pid = 0;
                break;
            }
        }
        if (!is_pid || dirs[i].d_name[0] == '\0')
            continue;

        char status_path[64];
        int k = 0;
        const char *prefix = "/proc/";
        while (*prefix && k < 63) status_path[k++] = *prefix++;
        for (int j = 0; dirs[i].d_name[j] && k < 63; j++)
            status_path[k++] = dirs[i].d_name[j];
        const char *suffix = "/status";
        while (*suffix && k < 63) status_path[k++] = *suffix++;
        status_path[k] = '\0';

        int fd = open(status_path, 0);
        if (fd < 0) continue;

        char buf[256];
        int nr = read(fd, buf, 255);
        close(fd);
        if (nr <= 0) continue;
        buf[nr] = '\0';

        char pid_str[16] = {0};
        char state_str[16] = {0};
        char name_str[64] = {0};
        int pid_val = 0;

        const char *line = buf;
        while (*line) {
            const char *nl = line;
            while (*nl && *nl != '\n') nl++;
            int len = nl - line;
            if (len > 4 && line[0] == 'N' && line[1] == 'a' &&
                line[2] == 'm' && line[3] == 'e' && line[4] == ':') {
                const char *v = line + 5;
                while (*v == ' ' || *v == '\t') v++;
                int vi = 0;
                while (v[vi] && vi < 63 && v[vi] != '\n' && v[vi] != '\r') {
                    name_str[vi] = v[vi]; vi++;
                }
                name_str[vi] = '\0';
            }
            if (len > 4 && line[0] == 'P' && line[1] == 'i' &&
                line[2] == 'd' && line[3] == ':') {
                const char *v = line + 5;
                while (*v == ' ' || *v == '\t') v++;
                pid_val = 0;
                while (*v >= '0' && *v <= '9') {
                    pid_val = pid_val * 10 + (*v - '0'); v++;
                }
            }
            if (len > 6 && line[0] == 'S' && line[1] == 't' &&
                line[2] == 'a' && line[3] == 't' && line[4] == 'e') {
                const char *v = line + 5;
                while (*v == ' ' || *v == '\t' || *v == ':') v++;
                int vi = 0;
                while (v[vi] && vi < 15 && v[vi] != '\n' && v[vi] != '\r') {
                    state_str[vi] = v[vi]; vi++;
                }
                state_str[vi] = '\0';
            }
            line = nl + 1;
        }

        printf("%-6d %-10s %s\n", pid_val, state_str, name_str);
    }
    return 0;
}
#endif

#ifdef CMD_MKDIR
static int mkdir_p(char *path, unsigned int mode) {
    char *p = path;
    while (*p == '/') p++;
    if (!*p) return 0;
    while (1) {
        char *slash = strchr(p, '/');
        if (slash) *slash = '\0';
        if (mkdir(path, mode) < 0) {
            if (access(path, 0) < 0) return -1;
        }
        if (!slash) break;
        *slash = '/';
        p = slash + 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int flags = 0;
    unsigned int mode = 0777;
    int i, j;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') break;
        j = 1;
        while (argv[i][j]) {
            if (argv[i][j] == 'p') {
                flags |= 1;
            } else if (argv[i][j] == 'v') {
                flags |= 2;
            } else if (argv[i][j] == 'm') {
                j++;
                char *ms = argv[i] + j;
                if (*ms) {
                    mode = 0;
                    while (*ms >= '0' && *ms <= '7') { mode = mode * 8 + (*ms - '0'); ms++; }
                } else if (i + 1 < argc) {
                    i++;
                    ms = argv[i];
                    mode = 0;
                    while (*ms >= '0' && *ms <= '7') { mode = mode * 8 + (*ms - '0'); ms++; }
                }
                goto next_arg;
            } else {
                printf("mkdir: invalid option -- '%c'\n", argv[i][j]);
                return 1;
            }
            j++;
        }
next_arg:
        ;
    }

    if (i >= argc) {
        printf("mkdir: missing operand\n");
        return 1;
    }

    int status = 0;
    for (; i < argc; i++) {
        int ret;
        if (flags & 1) {
            char path[256];
            strncpy(path, argv[i], 256);
            path[255] = '\0';
            ret = mkdir_p(path, mode);
        } else {
            ret = mkdir(argv[i], mode);
        }
        if (ret < 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[i]);
            status = 1;
        } else if (flags & 2) {
            printf("mkdir: created directory '%s'\n", argv[i]);
        }
    }
    return status;
}
#endif

#ifdef CMD_RM
static int rm_rf(const char *path, int flags) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (flags & 1) return 0;
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!(flags & 2)) {
            printf("rm: cannot remove '%s': Is a directory\n", path);
            return -1;
        }
        struct dirent dirs[32];
        int n = getdents(path, dirs, 32);
        if (n < 0) return -1;

        for (int i = 0; i < n; i++) {
            if (dirs[i].d_name[0] == '.' &&
                (dirs[i].d_name[1] == '\0' ||
                 (dirs[i].d_name[1] == '.' && dirs[i].d_name[2] == '\0')))
                continue;
            char fullpath[256];
            int plen = strlen(path);
            int k;
            for (k = 0; k < plen && k < 255; k++) fullpath[k] = path[k];
            if (k > 0 && fullpath[k-1] != '/' && k < 255) fullpath[k++] = '/';
            for (int j = 0; dirs[i].d_name[j] && k < 255; j++, k++)
                fullpath[k] = dirs[i].d_name[j];
            fullpath[k] = '\0';
            rm_rf(fullpath, flags);
        }

        if (rmdir(path) < 0) return -1;
        if (flags & 4) printf("removed directory '%s'\n", path);
        return 0;
    }

    if (unlink(path) < 0) return -1;
    if (flags & 4) printf("removed '%s'\n", path);
    return 0;
}

int main(int argc, char **argv) {
    int flags = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') break;
        int j = 1;
        while (argv[i][j]) {
            if (argv[i][j] == 'f') flags |= 1;
            else if (argv[i][j] == 'r' || argv[i][j] == 'R') flags |= 2;
            else if (argv[i][j] == 'v') flags |= 4;
            else {
                printf("rm: invalid option -- '%c'\n", argv[i][j]);
                return 1;
            }
            j++;
        }
    }

    if (i >= argc) {
        printf("rm: missing operand\n");
        return 1;
    }

    int status = 0;
    for (; i < argc; i++) {
        if (rm_rf(argv[i], flags) < 0 && !(flags & 1)) {
            printf("rm: cannot remove '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
#endif

#ifdef CMD_STAT
static void print_mode(unsigned int mode) {
    if (S_ISREG(mode)) putchar('-');
    else if (S_ISDIR(mode)) putchar('d');
    else if (S_ISCHR(mode)) putchar('c');
    else if (S_ISBLK(mode)) putchar('b');
    else if (S_ISLNK(mode)) putchar('l');
    else if (S_ISFIFO(mode)) putchar('p');
    else if (S_ISSOCK(mode)) putchar('s');
    else putchar('?');
    putchar((mode & S_IRUSR) ? 'r' : '-');
    putchar((mode & S_IWUSR) ? 'w' : '-');
    putchar((mode & S_IXUSR) ? 'x' : '-');
    putchar((mode & S_IRGRP) ? 'r' : '-');
    putchar((mode & S_IWGRP) ? 'w' : '-');
    putchar((mode & S_IXGRP) ? 'x' : '-');
    putchar((mode & S_IROTH) ? 'r' : '-');
    putchar((mode & S_IWOTH) ? 'w' : '-');
    putchar((mode & S_IXOTH) ? 'x' : '-');
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("stat: missing operand\n");
        return 1;
    }
    struct stat st;
    if (stat(argv[1], &st) < 0) {
        printf("stat: %s: not found\n", argv[1]);
        return 1;
    }
    print_mode(st.st_mode);
    printf("  inode=%u  links=%u  uid=%u  gid=%u  size=%u  blks=%u\n",
           st.st_ino, st.st_nlink, st.st_uid, st.st_gid,
           st.st_size, st.st_blocks);
    return 0;
}
#endif

#ifdef CMD_WHOAMI
int main(void) {
    printf("%u\n", geteuid());
    return 0;
}
#endif

#ifdef CMD_ID
int main(void) {
    printf("uid=%u gid=%u euid=%u egid=%u\n",
           getuid(), getgid(), geteuid(), getegid());
    return 0;
}
#endif

#ifdef CMD_TRUE
int main(void) { return 0; }
#endif

#ifdef CMD_FALSE
int main(void) { return 1; }
#endif

#ifdef CMD_YES
int main(int argc, char **argv) {
    const char *s = argc > 1 ? argv[1] : "y";
    for (;;) printf("%s\n", s);
    return 0;
}
#endif

#ifdef CMD_BASENAME
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *s = argv[1];
    const char *p = strrchr(s, '/');
    printf("%s\n", p ? p + 1 : s);
    return 0;
}
#endif

#ifdef CMD_DIRNAME
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *s = argv[1];
    const char *p = strrchr(s, '/');
    if (!p) {
        printf(".\n");
    } else if (p == s) {
        printf("/\n");
    } else {
        char buf[256];
        int len = p - s;
        for (int i = 0; i < len && i < 255; i++) buf[i] = s[i];
        buf[len] = '\0';
        printf("%s\n", buf);
    }
    return 0;
}
#endif

#ifdef CMD_WC
int main(int argc, char **argv) {
    int fd = (argc > 1) ? open(argv[1], 0) : 0;
    if (fd < 0) {
        printf("wc: %s: not found\n", argv[1]);
        return 1;
    }
    unsigned int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            bytes++;
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    if (fd != 0) close(fd);
    printf("%u %u %u", lines, words, bytes);
    if (argc > 1) printf(" %s", argv[1]);
    putchar('\n');
    return 0;
}
#endif

#ifdef CMD_SEQ
int main(int argc, char **argv) {
    int first = 1, last = 1, step = 1;
    if (argc > 1) last = atoi(argv[1]);
    if (argc > 2) { first = last; last = atoi(argv[2]); }
    if (argc > 3) { first = atoi(argv[1]); step = atoi(argv[2]); last = atoi(argv[3]); }
    if (step > 0) {
        for (int i = first; i <= last; i += step) printf("%d\n", i);
    } else if (step < 0) {
        for (int i = first; i >= last; i += step) printf("%d\n", i);
    }
    return 0;
}
#endif

#ifdef CMD_HEAD
int main(int argc, char **argv) {
    const char *path = NULL;
    unsigned int nlines = 10;
    int arg = 1;
    if (argc > arg && argv[arg][0] == '-' && argv[arg][1]) {
        nlines = (unsigned int)atoi(argv[arg] + 1);
        arg++;
    }
    if (argc > arg) path = argv[arg];
    int fd = path ? open(path, 0) : 0;
    if (fd < 0) {
        printf("head: %s: not found\n", path);
        return 1;
    }
    char buf[1024];
    int n, lines = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0 && (unsigned int)lines < nlines) {
        for (int i = 0; i < n && (unsigned int)lines < nlines; i++) {
            if (buf[i] == '\n') lines++;
        }
        write(1, buf, n);
    }
    if (fd != 0) close(fd);
    return 0;
}
#endif

#ifdef CMD_WHICH
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    for (int i = 1; i < argc; i++) {
        char path[64];
        int k = 0;
        const char *prefix = "/bin/";
        while (*prefix && k < 63) path[k++] = *prefix++;
        for (int j = 0; argv[i][j] && k < 63; j++) path[k++] = argv[i][j];
        path[k] = '\0';
        if (access(path, 0) == 0)
            printf("%s\n", path);
    }
    return 0;
}
#endif

#ifdef CMD_TEST
static int test_stat(const char *path, int mode_mask) {
    struct stat st;
    if (stat(path, &st) < 0) return 1;
    return (st.st_mode & S_IFMT) == (unsigned int)mode_mask ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (argc == 2) {
        return argv[1][0] ? 0 : 1;
    }
    if (argc == 3) {
        if (strcmp(argv[1], "!") == 0) return argv[2][0] ? 1 : 0;
        return 0;
    }
    if (argc == 4) {
        if (argv[1][0] == '-') {
            switch (argv[1][1]) {
            case 'f': return test_stat(argv[2], S_IFREG);
            case 'd': return test_stat(argv[2], S_IFDIR);
            case 'e': return access(argv[2], 0) == 0 ? 0 : 1;
            case 'r': return access(argv[2], 4) == 0 ? 0 : 1;
            case 'w': return access(argv[2], 2) == 0 ? 0 : 1;
            case 'x': return access(argv[2], 1) == 0 ? 0 : 1;
            case 's': {
                struct stat st;
                if (stat(argv[2], &st) < 0) return 1;
                return st.st_size > 0 ? 0 : 1;
            }
            default: return 1;
            }
        }
        if (strcmp(argv[2], "=") == 0) return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        if (strcmp(argv[2], "!=") == 0) return strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
        return 1;
    }
    return 1;
}
#endif

#ifdef CMD_CMP
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("cmp: missing operand\n");
        return 1;
    }
    int fda = open(argv[1], 0);
    int fdb = open(argv[2], 0);
    if (fda < 0 || fdb < 0) {
        printf("cmp: cannot open\n");
        return 1;
    }
    unsigned int off = 0;
    char bufa[512], bufb[512];
    for (;;) {
        int na = read(fda, bufa, sizeof(bufa));
        int nb = read(fdb, bufb, sizeof(bufb));
        int n = na < nb ? na : nb;
        for (int i = 0; i < n; i++, off++) {
            if (bufa[i] != bufb[i]) {
                printf("%s %s differ: byte %u\n", argv[1], argv[2], off);
                close(fda); close(fdb);
                return 1;
            }
        }
        if (na == 0 && nb == 0) break;
        if (na != nb) {
            printf("%s %s differ: EOF on %s at byte %u\n",
                   argv[1], argv[2], na < nb ? argv[1] : argv[2], off);
            close(fda); close(fdb);
            return 1;
        }
    }
    close(fda); close(fdb);
    return 0;
}
#endif

#ifdef CMD_HEXDUMP
int main(int argc, char **argv) {
    int fd = argc > 1 ? open(argv[1], 0) : 0;
    if (fd < 0) {
        printf("hexdump: %s: not found\n", argv[1]);
        return 1;
    }
    unsigned int off = 0;
    char buf[16];
    int n;
    while ((n = read(fd, buf, 16)) > 0) {
        printf("%08x  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", (unsigned char)buf[i]);
            else printf("   ");
            if (i == 7) putchar(' ');
        }
        putchar(' ');
        for (int i = 0; i < n; i++)
            putchar((buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.');
        putchar('\n');
        off += n;
    }
    if (fd != 0) close(fd);
    return 0;
}
#endif

#ifdef CMD_HOSTNAME
int main(void) {
    printf("localhost\n");
    return 0;
}
#endif
