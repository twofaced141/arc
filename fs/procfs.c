#include "procfs.h"
#include "process.h"
#include "pmm.h"
#include "pit.h"
#include "debug.h"
#include "scheduler.h"
#include <stddef.h>
#include <string.h>

static int is_digit(char c) { return c >= '0' && c <= '9'; }

int procfs_is_proc_path(const char *path) {
    if (!path) return 0;
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
        path[3] == 'o' && path[4] == 'c' && (path[5] == '\0' || path[5] == '/'))
        return 1;
    return 0;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int parse_pid(const char *path, uint32_t *pid) {
    const char *p = path + 6;
    while (*p == '/') p++;
    if (!*p || !is_digit(*p)) return -1;
    uint32_t n = 0;
    while (is_digit(*p)) { n = n * 10 + (uint32_t)(*p - '0'); p++; }
    *pid = n;
    return 0;
}

static const char *skip_pid(const char *path) {
    const char *p = path + 6;
    while (*p == '/') p++;
    while (is_digit(*p)) p++;
    if (*p == '/') p++;
    return p;
}

static int append(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    return pos;
}

static int append_dec(char *buf, int pos, int max, uint32_t val) {
    char tmp[16];
    int i = sizeof(tmp) - 1;
    tmp[i] = '\0';
    if (val == 0) { tmp[--i] = '0'; }
    else { while (val) { tmp[--i] = '0' + (val % 10); val /= 10; } }
    while (tmp[i] && pos < max - 1) buf[pos++] = tmp[i++];
    return pos;
}

static int append_nl(char *buf, int pos, int max) {
    if (pos < max - 1) buf[pos++] = '\n';
    return pos;
}

static int generate_cpuinfo(char *buf, int max) {
    int p = 0;
    p = append(buf, p, max, "processor       : 0\n");
    p = append(buf, p, max, "vendor_id       : GenuineIntel\n");
    p = append(buf, p, max, "model           : 0\n");
    p = append(buf, p, max, "flags           : fpu\n");
    return (p < max) ? p : max;
}

static int generate_meminfo(char *buf, int max) {
    uint32_t total = pmm_get_total_pages() * 4;
    uint32_t free  = pmm_get_free_pages() * 4;
    uint32_t used  = total - free;
    int p = 0;
    p = append(buf, p, max, "MemTotal:   ");
    p = append_dec(buf, p, max, total);
    p = append(buf, p, max, " kB\nMemFree:    ");
    p = append_dec(buf, p, max, free);
    p = append(buf, p, max, " kB\nMemUsed:    ");
    p = append_dec(buf, p, max, used);
    p = append(buf, p, max, " kB\n");
    return (p < max) ? p : max;
}

static int generate_uptime(char *buf, int max) {
    uint32_t s = pit_get_ticks() / 100;
    int p = 0;
    p = append_dec(buf, p, max, s);
    p = append(buf, p, max, ".00 ");
    p = append_dec(buf, p, max, s);
    p = append(buf, p, max, ".00\n");
    return (p < max) ? p : max;
}

static int generate_version(char *buf, int max) {
    return append(buf, 0, max,
        "opencodeOS version 0.1.0 (gcc) #1 Tue Jun 30 2026\n");
}

static int generate_stat(char *buf, int max) {
    int nprocs = 0, nrunning = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state != PROC_UNUSED) {
            nprocs++;
            if (processes[i].state == PROC_READY || processes[i].state == PROC_RUNNING)
                nrunning++;
        }
    }
    int p = 0;
    p = append(buf, p, max, "cpu  ");
    p = append_dec(buf, p, max, pit_get_ticks());
    p = append(buf, p, max, " 0 0 0\nprocesses ");
    p = append_dec(buf, p, max, nprocs);
    p = append(buf, p, max, "\nprocs_running ");
    p = append_dec(buf, p, max, nrunning);
    p = append_nl(buf, p, max);
    return (p < max) ? p : max;
}

static int generate_status(char *buf, int max, uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid != pid || processes[i].state == PROC_UNUSED)
            continue;
        process_t *p = &processes[i];
        const char *state_str = "unknown";
        switch (p->state) {
        case PROC_READY:   state_str = "Ready";    break;
        case PROC_RUNNING: state_str = "Running";  break;
        case PROC_BLOCKED: state_str = "Blocked";  break;
        case PROC_ZOMBIE:  state_str = "Zombie";   break;
        }
        uint32_t heap_kb = (p->heap_break - USER_HEAP_START) / 1024;

        int pos = 0;
        pos = append(buf, pos, max, "Name:   ");
        pos = append(buf, pos, max, p->name);
        pos = append_nl(buf, pos, max);

        pos = append(buf, pos, max, "Pid:    ");
        pos = append_dec(buf, pos, max, p->pid);
        pos = append_nl(buf, pos, max);

        pos = append(buf, pos, max, "State:  ");
        pos = append(buf, pos, max, state_str);
        pos = append_nl(buf, pos, max);

        pos = append(buf, pos, max, "Uid:    ");
        pos = append_dec(buf, pos, max, p->uid);
        pos = append(buf, pos, max, " ");
        pos = append_dec(buf, pos, max, p->euid);
        pos = append(buf, pos, max, " 0 0\n");

        pos = append(buf, pos, max, "Gid:    ");
        pos = append_dec(buf, pos, max, p->gid);
        pos = append(buf, pos, max, " ");
        pos = append_dec(buf, pos, max, p->egid);
        pos = append(buf, pos, max, " 0 0\n");

        pos = append(buf, pos, max, "VmSize: ");
        pos = append_dec(buf, pos, max, heap_kb);
        pos = append(buf, pos, max, " kB\n");

        return (pos < max) ? pos : max;
    }
    int pos = 0;
    pos = append(buf, pos, max, "PID ");
    pos = append_dec(buf, pos, max, pid);
    pos = append(buf, pos, max, " not found\n");
    return (pos < max) ? pos : max;
}

static int generate_mem(char *buf, int max, uint32_t pid) {
    int p = 0;
    p = append(buf, p, max,
        "address          perm  offset   device  inode\n"
        "08000000-08001000 r-xp  00000000 00:00   ");
    p = append_dec(buf, p, max, pid);
    p = append(buf, p, max, "\nBFFFF000-C0000000 rw-p  00000000 00:00   0\n");
    return (p < max) ? p : max;
}

int procfs_open(const char *path, proc_file_t **out) {
    proc_file_t *pf = (proc_file_t *)kmalloc(sizeof(proc_file_t));
    if (!pf) return -1;
    pf->content = NULL;
    pf->size = 0;
    pf->pid = 0;
    pf->subtype = 0;

    char *content = (char *)kmalloc(PROC_MAX_CONTENT);
    if (!content) { kfree(pf); return -1; }

    int n = 0;

    if (streq(path, "/proc/cpuinfo") || streq(path, "/proc/cpuinfo/")) {
        pf->type = PROC_CPUINFO;
        n = generate_cpuinfo(content, PROC_MAX_CONTENT);
    } else if (streq(path, "/proc/meminfo") || streq(path, "/proc/meminfo/")) {
        pf->type = PROC_MEMINFO;
        n = generate_meminfo(content, PROC_MAX_CONTENT);
    } else if (streq(path, "/proc/uptime") || streq(path, "/proc/uptime/")) {
        pf->type = PROC_UPTIME;
        n = generate_uptime(content, PROC_MAX_CONTENT);
    } else if (streq(path, "/proc/version") || streq(path, "/proc/version/")) {
        pf->type = PROC_VERSION;
        n = generate_version(content, PROC_MAX_CONTENT);
    } else if (streq(path, "/proc/stat") || streq(path, "/proc/stat/")) {
        pf->type = PROC_STAT;
        n = generate_stat(content, PROC_MAX_CONTENT);
    } else if (parse_pid(path, &pf->pid) == 0) {
        pf->type = PROC_PID_DIR;
        const char *sub = skip_pid(path);
        if (*sub) {
            if (streq(sub, "status")) {
                pf->subtype = PROC_PID_STATUS;
                n = generate_status(content, PROC_MAX_CONTENT, pf->pid);
            } else if (streq(sub, "mem")) {
                pf->subtype = PROC_PID_MEM;
                n = generate_mem(content, PROC_MAX_CONTENT, pf->pid);
            } else {
                kfree(content); kfree(pf); return -1;
            }
        } else {
            kfree(content); kfree(pf); return -1;
        }
    } else {
        kfree(content); kfree(pf); return -1;
    }

    if (n <= 0) { kfree(content); kfree(pf); return -1; }
    if (n >= PROC_MAX_CONTENT) n = PROC_MAX_CONTENT - 1;
    content[n] = '\0';
    pf->content = content;
    pf->size = (uint32_t)n;
    *out = pf;
    return 0;
}

void procfs_close(proc_file_t *pf) {
    if (!pf) return;
    if (pf->content) kfree(pf->content);
    kfree(pf);
}

int procfs_readdir(const char *path, char *buf, uint32_t size, uint32_t *out_bytes) {
    uint32_t bytes = 0;

    if (streq(path, "/proc") || streq(path, "/proc/")) {
        const char *top[] = {"cpuinfo", "meminfo", "uptime", "version", "stat", NULL};
        for (int i = 0; top[i]; i++) {
            const char *s = top[i];
            while (*s && bytes < size) buf[bytes++] = *s++;
            if (bytes < size) buf[bytes++] = '\n';
        }
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (processes[i].state == PROC_UNUSED) continue;
            char pid_str[16];
            int pid_len = 0;
            uint32_t pid = processes[i].pid;
            if (pid == 0) { pid_str[0] = '0'; pid_len = 1; }
            else {
                uint32_t tmp = pid;
                while (tmp) { tmp /= 10; pid_len++; }
                int k = pid_len;
                tmp = pid;
                while (k) { pid_str[--k] = '0' + (tmp % 10); tmp /= 10; }
            }
            for (int k = 0; k < pid_len && bytes < size; k++)
                buf[bytes++] = pid_str[k];
            if (bytes < size) buf[bytes++] = '\n';
        }
        *out_bytes = bytes;
        return 0;
    }

    uint32_t pid;
    if (parse_pid(path, &pid) == 0) {
        int found = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (processes[i].pid == pid && processes[i].state != PROC_UNUSED)
                { found = 1; break; }
        }
        if (!found) return -1;

        const char *files[] = {"status", "mem", NULL};
        for (int i = 0; files[i]; i++) {
            const char *s = files[i];
            while (*s && bytes < size) buf[bytes++] = *s++;
            if (bytes < size) buf[bytes++] = '\n';
        }
        *out_bytes = bytes;
        return 0;
    }

    return -1;
}

int procfs_getdents(const char *path, struct dirent *dirp, uint32_t count) {
    uint32_t written = 0;

    if (streq(path, "/proc") || streq(path, "/proc/")) {
        struct { const char *name; uint8_t type; } top[] = {
            {"cpuinfo", 1}, {"meminfo", 1}, {"uptime", 1},
            {"version", 1}, {"stat", 1}, {NULL, 0}
        };
        for (int i = 0; top[i].name && written < count; i++) {
            struct dirent *d = &dirp[written];
            d->d_ino = 0;
            d->d_off = written;
            d->d_type = top[i].type;
            d->d_reclen = sizeof(struct dirent);
            int j = 0;
            while (top[i].name[j] && j < 255) { d->d_name[j] = top[i].name[j]; j++; }
            d->d_name[j] = '\0';
            written++;
        }
        for (int i = 0; i < MAX_PROCESSES && written < count; i++) {
            if (processes[i].state == PROC_UNUSED) continue;
            struct dirent *d = &dirp[written];
            d->d_ino = processes[i].pid;
            d->d_off = written;
            d->d_type = 2;
            d->d_reclen = sizeof(struct dirent);
            uint32_t pid = processes[i].pid;
            char pid_str[16];
            int pid_len = 0;
            if (pid == 0) { pid_str[0] = '0'; pid_len = 1; }
            else {
                uint32_t tmp = pid;
                while (tmp) { tmp /= 10; pid_len++; }
                int k = pid_len;
                tmp = pid;
                while (k) { pid_str[--k] = '0' + (tmp % 10); tmp /= 10; }
            }
            for (int j = 0; j < pid_len && j < 255; j++)
                d->d_name[j] = pid_str[j];
            d->d_name[pid_len] = '\0';
            written++;
        }
        return (int)written;
    }

    uint32_t pid;
    if (parse_pid(path, &pid) == 0) {
        int found = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (processes[i].pid == pid && processes[i].state != PROC_UNUSED)
                { found = 1; break; }
        }
        if (!found) return -1;

        struct { const char *name; uint8_t type; } files[] = {
            {"status", 1}, {"mem", 1}, {NULL, 0}
        };
        for (int i = 0; files[i].name && written < count; i++) {
            struct dirent *d = &dirp[written];
            d->d_ino = pid;
            d->d_off = written;
            d->d_type = files[i].type;
            d->d_reclen = sizeof(struct dirent);
            int j = 0;
            while (files[i].name[j] && j < 255) { d->d_name[j] = files[i].name[j]; j++; }
            d->d_name[j] = '\0';
            written++;
        }
        return (int)written;
    }

    return -1;
}
