/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


/* init.c — arc PID 1.
 *
 * Boot policy:
 *   1. Scan /etc/rc/ for *.rc units (rc_scan_dir).
 *   2. Valid units found -> service boot:
 *        - validate deps (unknown names / later-phase deps -> warnings)
 *        - topological order within each phase (mounts, oneshots,
 *          services); dependency cycles are logged and broken
 *        - run mounts, then oneshots, then spawn services
 *        - supervise: reap children, respawn per policy with backoff
 *          and a restart limit; a healthy run resets the counter
 *      Error handling is non-fatal by design: a failed unit is logged,
 *      marked failed, and its dependents are skipped with a log entry.
 *      No unit failure ever aborts the boot.
 *   3. No units -> bare boot: announce the reason, run the built-in
 *      process self-test once, then keep waiting for the filesystem
 *      (re-scan every RC_RETRY_SECS).  A directory full of broken
 *      files is not retried — idle instead.
 */

#include "../syscall.h"
#include "../rc/rcparse.h"

/* Minimal errno values (kernel returns negative errno) */
#define ERR_ENOENT 2
#define ERR_ECHILD 10
#define ERR_EEXIST 17

/* Respawn policy limits */
#define RC_MAX_RESTARTS  5       /* consecutive restarts before giving up */
#define RC_BACKOFF_MAX   4       /* seconds, doubles: 1,2,4,4,... */
#define RC_RESET_SECS    10      /* healthy run length that resets counter */

#define RC_DIR       "/etc/rc"
#define RC_RETRY_SECS 5


static long bsd_write(int fd, const void *buf, unsigned long cnt) {
    return syscall3(BSD_SYS(SYS_WRITE), fd, (long)buf, cnt);
}
static long bsd_exit(long code) {
    return syscall1(BSD_SYS(SYS_EXIT), code);
}
static long bsd_sleep(unsigned long seconds) {
    return syscall1(BSD_SYS(SYS_SLEEP), seconds);
}
static long bsd_getpid(void) {
    return syscall0(BSD_SYS(SYS_GETPID));
}
static long bsd_fork(void) {
    return syscall0(BSD_SYS(SYS_FORK));
}
static long bsd_waitpid(long pid, int *status, int options) {
    return syscall3(BSD_SYS(SYS_WAITPID), pid, (long)status, options);
}
static long bsd_execve(const char *path, char **argv, char **envp) {
    return syscall3(BSD_SYS(SYS_EXECVE), (long)path, (long)argv, (long)envp);
}
static long bsd_open(const char *path, int flags) {
    return syscall3(BSD_SYS(SYS_OPEN), (long)path, flags, 0);
}
static long bsd_close(long fd) {
    return syscall1(BSD_SYS(SYS_CLOSE), fd);
}
static long bsd_dup2(int oldfd, int newfd) {
    return syscall2(BSD_SYS(SYS_DUP2), oldfd, newfd);
}
static long bsd_setuid(int uid) {
    return syscall1(BSD_SYS(SYS_SETUID), uid);
}
static long bsd_setgid(int gid) {
    return syscall1(BSD_SYS(SYS_SETGID), gid);
}
static long bsd_mkdir(const char *path, int mode) {
    return syscall2(BSD_SYS(SYS_MKDIR), (long)path, mode);
}
static long bsd_mount(const char *dev, const char *path) {
    return syscall2(BSD_SYS(SYS_MOUNT), (long)dev, (long)path);
}
static long bsd_uptime(void) {
    return syscall0(BSD_SYS(SYS_UPTIME));
}


static unsigned long strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void print(const char *s) {
    bsd_write(1, s, strlen(s));
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


enum {
    U_PENDING = 0,   /* not yet started */
    U_RUNNING,       /* service running (or mount in progress) */
    U_OK,            /* mount/oneshot succeeded */
    U_FAILED,        /* mount/oneshot/service failed to start */
    U_SKIPPED,       /* skipped: a dependency failed */
    U_EXITED,        /* service exited, policy says no respawn */
    U_GAVEUP,        /* respawn limit reached */
};

typedef struct rtu {
    rc_unit_t *u;
    int  state;
    long pid;            /* service pid while running */
    long start_secs;     /* uptime at last start */
    int  restarts;       /* consecutive restarts */
    int  backoff;        /* current backoff in seconds */
} rtu_t;

static rc_unit_t rc_units[RC_UNITS_MAX];
static rtu_t     rt[RC_UNITS_MAX];
static int       rt_count;

static int rc_load(void) {
    return rc_scan_dir(RC_DIR, rc_units, RC_UNITS_MAX);
}

/* Fill the runtime table from the freshly loaded unit array. */
static void rt_build(int n) {
    for (int i = 0; i < n; i++) {
        rt[i].u = &rc_units[i];
        rt[i].state = U_PENDING;
        rt[i].pid = 0;
        rt[i].start_secs = 0;
        rt[i].restarts = 0;
        rt[i].backoff = 1;
    }
    rt_count = n;
}

static int phase_of(const rc_unit_t *u) {
    switch (u->type) {
    case RC_T_MOUNT:   return 1;
    case RC_T_ONESHOT: return 2;
    default:           return 3;   /* RC_T_SERVICE */
    }
}

static rtu_t *unit_by_name(const char *name) {
    for (int i = 0; i < rt_count; i++)
        if (strcmp(rt[i].u->name, name) == 0)
            return &rt[i];
    return NULL;
}


static void validate_deps(void) {
    for (int i = 0; i < rt_count; i++) {
        rc_unit_t *u = rt[i].u;
        for (int d = 0; d < u->ndeps; d++) {
            rtu_t *j = unit_by_name(u->deps[d]);
            if (!j) {
                print("init: warning: "); print(u->name);
                print(": dep '"); print(u->deps[d]);
                print("' not found — ignored\n");
            } else if (phase_of(j->u) > phase_of(u)) {
                print("init: warning: "); print(u->name);
                print(": dep '"); print(u->deps[d]);
                print("' runs in a later phase — order not guaranteed\n");
            }
        }
        for (int d = 0; d < u->nafter; d++) {
            if (!unit_by_name(u->after[d])) {
                print("init: warning: "); print(u->name);
                print(": after '"); print(u->after[d]);
                print("' not found — ignored\n");
            }
        }
    }
}

/* Kahn's algorithm over list[]; deps and after are ordering edges.
 * Cycles are logged and broken (remaining units keep file order).
 * Static scratch — init is single-threaded, avoid 16KB user stack. */
static unsigned char topo_adj[RC_UNITS_MAX][RC_UNITS_MAX];
static char   topo_done[RC_UNITS_MAX];
static int    topo_indeg[RC_UNITS_MAX];
static int    topo_order[RC_UNITS_MAX];
static rtu_t *topo_tmp[RC_UNITS_MAX];

static void topo_sort(rtu_t **list, int n) {
    for (int i = 0; i < n; i++) {
        topo_indeg[i] = 0;
        topo_done[i] = 0;
        for (int k = 0; k < n; k++)
            topo_adj[i][k] = 0;
    }

    /* Edge: list[i] must precede list[k] when list[k] names list[i]. */
    for (int k = 0; k < n; k++) {
        rc_unit_t *u = list[k]->u;
        for (int d = 0; d < u->ndeps; d++) {
            rtu_t *dep = unit_by_name(u->deps[d]);
            if (!dep) continue;
            for (int i = 0; i < n; i++)
                if (list[i] == dep) { topo_adj[i][k] = 1; break; }
        }
        for (int d = 0; d < u->nafter; d++) {
            rtu_t *dep = unit_by_name(u->after[d]);
            if (!dep) continue;
            for (int i = 0; i < n; i++)
                if (list[i] == dep) { topo_adj[i][k] = 1; break; }
        }
    }
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++)
            if (topo_adj[i][k])
                topo_indeg[k]++;

    int oc = 0;
    for (;;) {
        int p = -1;
        for (int i = 0; i < n; i++)
            if (!topo_done[i] && topo_indeg[i] == 0) { p = i; break; }
        if (p < 0)
            break;
        topo_done[p] = 1;
        topo_order[oc++] = p;
        for (int k = 0; k < n; k++)
            if (!topo_done[k] && topo_adj[p][k])
                topo_indeg[k]--;
    }
    if (oc < n) {
        print("init: dependency cycle among: ");
        for (int i = 0; i < n; i++)
            if (!topo_done[i]) { print(list[i]->u->name); print(" "); }
        print("— breaking, starting in file order\n");
        for (int i = 0; i < n; i++)
            if (!topo_done[i])
                topo_order[oc++] = i;
    }
    for (int i = 0; i < n; i++)
        topo_tmp[i] = list[topo_order[i]];
    for (int i = 0; i < n; i++)
        list[i] = topo_tmp[i];
}

/* Hard deps: a unit is blocked when a named dep ended in a bad state
 * (failed/skipped/exited/gave-up) or runs in a later phase. */
static int dep_blocked(rtu_t *t) {
    for (int d = 0; d < t->u->ndeps; d++) {
        rtu_t *j = unit_by_name(t->u->deps[d]);
        if (!j)
            continue;                       /* unknown — warned in validate */
        if (j->state == U_FAILED || j->state == U_SKIPPED ||
            j->state == U_EXITED || j->state == U_GAVEUP) {
            print("init: "); print(t->u->name); print(": dep '");
            print(t->u->deps[d]); print("' failed — skipping\n");
            return 1;
        }
        if (j->state == U_PENDING && phase_of(j->u) > phase_of(t->u)) {
            print("init: "); print(t->u->name); print(": dep '");
            print(t->u->deps[d]); print("' runs later — skipping\n");
            return 1;
        }
    }
    return 0;
}


static int st_exit_code(int st) { return (st >> 8) & 0xff; }
static int st_sig(int st)       { return st & 0x7f; }


static void child_setup(rc_unit_t *u) {
    if (u->stdio == RC_IO_SILENT) {
        long fd = bsd_open("/dev/null", 2 /* O_RDWR */);
        if (fd < 0) {
            print("init: "); print(u->name);
            print(": cannot open /dev/null\n");
            bsd_exit(126);
        }
        bsd_dup2((int)fd, 0);
        bsd_dup2((int)fd, 1);
        bsd_dup2((int)fd, 2);
        bsd_close(fd);
    }
    if (u->gid && bsd_setgid(u->gid) < 0) {
        print("init: "); print(u->name); print(": setgid failed\n");
        bsd_exit(126);
    }
    if (u->uid && bsd_setuid(u->uid) < 0) {
        print("init: "); print(u->name); print(": setuid failed\n");
        bsd_exit(126);
    }
}

/* Build argv/envp and exec.  On failure prints and exits 127. */
static void child_exec(rc_unit_t *u) {
    char *argv[RC_ARG_MAX + 2];
    char *envp[RC_ENV_MAX + 1];
    int ac = 0;
    argv[ac++] = (char *)u->exec;
    for (int i = 0; i < u->argc; i++)
        argv[ac++] = u->argv[i];
    argv[ac] = NULL;
    int ec = 0;
    for (int i = 0; i < u->nenv; i++)
        envp[ec++] = u->env[i];
    envp[ec] = NULL;

    long r = bsd_execve(u->exec, argv, envp);
    print("init: "); print(u->name); print(": exec "); print(u->exec);
    print(" failed ("); print_dec(r); print(")\n");
    bsd_exit(127);
}


static void run_mount(rtu_t *t) {
    rc_unit_t *u = t->u;
    /* Best-effort: create the mountpoint so the parent fs shows it even
     * if the mount itself fails.  EEXIST is expected on re-run. */
    long r = bsd_mkdir(u->mountpoint, 0755);
    if (r < 0 && r != -ERR_EEXIST) {
        print("init: "); print(u->name); print(": mkdir ");
        print(u->mountpoint); print(" failed ("); print_dec(r);
        print(") — continuing\n");
    }
    r = bsd_mount(u->device, u->mountpoint);
    if (r < 0) {
        print("init: "); print(u->name); print(": mount ");
        print(u->device); print(" -> "); print(u->mountpoint);
        print(" failed ("); print_dec(r); print(")\n");
        t->state = U_FAILED;
    } else {
        print("init: mounted "); print(u->device); print(" -> ");
        print(u->mountpoint);
        if (u->fstype[0]) { print(" ("); print(u->fstype); print(")"); }
        print("\n");
        t->state = U_OK;
    }
}

static void run_oneshot(rtu_t *t) {
    long pid = bsd_fork();
    if (pid < 0) {
        print("init: "); print(t->u->name); print(": fork failed (");
        print_dec(pid); print(")\n");
        t->state = U_FAILED;
        return;
    }
    if (pid == 0) {
        child_setup(t->u);
        child_exec(t->u);        /* never returns on success */
        bsd_exit(127);
    }
    int status = 0;
    long r = bsd_waitpid(pid, &status, 0);
    if (r < 0) {
        print("init: "); print(t->u->name); print(": waitpid failed (");
        print_dec(r); print(")\n");
        t->state = U_FAILED;
        return;
    }
    int code = st_exit_code(status);
    int sig  = st_sig(status);
    print("init: oneshot "); print(t->u->name); print(": ");
    if (sig) {
        print("killed by signal "); print_dec(sig); print("\n");
        t->state = U_FAILED;
    } else if (code == 0) {
        print("ok\n");
        t->state = U_OK;
    } else {
        print("failed (exit "); print_dec(code); print(")\n");
        t->state = U_FAILED;
    }
}

static void spawn_service(rtu_t *t) {
    long pid = bsd_fork();
    if (pid < 0) {
        print("init: "); print(t->u->name); print(": fork failed (");
        print_dec(pid); print(")\n");
        t->state = U_FAILED;
        return;
    }
    if (pid == 0) {
        child_setup(t->u);
        child_exec(t->u);        /* never returns on success */
        bsd_exit(127);
    }
    t->pid = pid;
    t->state = U_RUNNING;
    t->start_secs = bsd_uptime();
    print("init: started "); print(t->u->name); print(" (pid=");
    print_dec(pid); print(")\n");
}

static void run_phase(int type) {
    rtu_t *list[RC_UNITS_MAX];
    int n = 0;
    for (int i = 0; i < rt_count; i++)
        if (phase_of(rt[i].u) == type)
            list[n++] = &rt[i];
    topo_sort(list, n);
    for (int i = 0; i < n; i++) {
        rtu_t *t = list[i];
        if (dep_blocked(t)) {
            t->state = U_SKIPPED;
            continue;
        }
        switch (type) {
        case RC_T_MOUNT:   run_mount(t);    break;
        case RC_T_ONESHOT: run_oneshot(t);  break;
        default:           spawn_service(t); break;
        }
    }
}


static void idle_loop(void) __attribute__((noreturn));

static void supervise(void) {
    for (;;) {
        int status = 0;
        long pid = bsd_waitpid(-1, &status, 0);
        if (pid < 0) {
            if (pid == -ERR_ECHILD) {
                print("init: no supervised children left — idle\n");
                idle_loop();
            }
            print("init: supervisor: waitpid error "); print_dec(pid);
            print(" — retrying\n");
            bsd_sleep(1);
            continue;
        }

        rtu_t *t = NULL;
        for (int i = 0; i < rt_count; i++)
            if (rt[i].state == U_RUNNING && rt[i].pid == pid) {
                t = &rt[i];
                break;
            }
        if (!t) {
            print("init: reaped unknown pid "); print_dec(pid); print("\n");
            continue;
        }

        int  code = st_exit_code(status);
        int  sig  = st_sig(status);
        long run  = bsd_uptime() - t->start_secs;
        print("init: "); print(t->u->name);
        if (sig) {
            print(" killed by signal "); print_dec(sig);
        } else {
            print(" exited ("); print_dec(code); print(")");
        }
        print(" after "); print_dec(run); print("s\n");

        int want;
        switch (t->u->respawn) {
        case RC_R_NEVER:   want = 0; break;
        case RC_R_ALWAYS:  want = 1; break;
        default:           want = (sig != 0 || code != 0); break;
        }
        if (!want) {
            t->state = U_EXITED;
            continue;
        }

        /* Permanent failure: exec itself failed (126/127).  Retrying is
         * pointless — the binary is missing or not executable. */
        if (!sig && (code == 126 || code == 127)) {
            print("init: "); print(t->u->name);
            print(" failed permanently (exit "); print_dec(code);
            print(") — not restarting\n");
            t->state = U_GAVEUP;
            continue;
        }

        if (run >= RC_RESET_SECS) {
            t->restarts = 0;
            t->backoff = 1;
        }
        t->restarts++;
        if (t->restarts > RC_MAX_RESTARTS) {
            print("init: "); print(t->u->name); print(" gave up after ");
            print_dec(t->restarts - 1); print(" restarts\n");
            t->state = U_GAVEUP;
            continue;
        }
        print("init: respawning "); print(t->u->name); print(" in ");
        print_dec(t->backoff); print("s\n");
        bsd_sleep(t->backoff);
        if (t->backoff < RC_BACKOFF_MAX)
            t->backoff *= 2;
        spawn_service(t);
        if (t->state != U_RUNNING) {
            print("init: "); print(t->u->name); print(" respawn failed\n");
        }
    }
}


static void print_unit_list(void) {
    for (int i = 0; i < rt_count; i++) {
        print("  ");
        print(rt[i].u->name);
        print(" (");
        switch (rt[i].u->type) {
        case RC_T_MOUNT:   print("mount");   break;
        case RC_T_ONESHOT: print("oneshot"); break;
        default:           print("service"); break;
        }
        if (rt[i].u->exec[0]) {
            print(" ");
            print(rt[i].u->exec);
        }
        print(")\n");
    }
}

static void service_boot(void) __attribute__((noreturn));
static void service_boot(void) {
    print("init: service boot: ");
    print_dec(rt_count);
    print(" unit(s) loaded\n");
    print_unit_list();
    validate_deps();

    run_phase(RC_T_MOUNT);
    run_phase(RC_T_ONESHOT);
    run_phase(RC_T_SERVICE);

    int running = 0;
    for (int i = 0; i < rt_count; i++)
        if (rt[i].state == U_RUNNING)
            running++;
    if (running == 0) {
        print("init: no services running — idle\n");
        idle_loop();
    }
    print("init: supervising "); print_dec(running); print(" service(s)\n");
    supervise();
    __builtin_unreachable();
}


/* One-shot diagnostic: fork, child exits 42, parent verifies via
 * waitpid status (kernel encodes WEXITSTATUS << 8, like BSD). */
static void proc_self_test(void) {
    print("init: proc self-test: fork ");
    long pid = bsd_fork();
    if (pid < 0) {
        print("FAILED (fork)\n");
        return;
    }
    if (pid == 0)
        bsd_exit(42);

    int status = 0;
    long r = bsd_waitpid(pid, &status, 0);
    if (r != pid) {
        print("FAILED (waitpid)\n");
        return;
    }
    if (((status >> 8) & 0xff) != 42) {
        print("FAILED (status)\n");
        return;
    }
    print("ok\n");
}

static void idle_loop(void) __attribute__((noreturn));
static void idle_loop(void) {
    for (;;)
        bsd_sleep(RC_RETRY_SECS);
    __builtin_unreachable();
}

static void bare_boot(int reason) __attribute__((noreturn));
static void bare_boot(int reason) {
    int retry = 0;

    switch (reason) {
    case RC_SCAN_NODIR:
        print("init: no /etc/rc directory — bare boot, waiting for fs\n");
        retry = 1;
        break;
    case 0:
        if (rc_last_error()[0]) {
            print("init: all rc units failed to parse: ");
            print(rc_last_error());
            print("\n");
        } else {
            print("init: /etc/rc is empty — bare boot, waiting for units\n");
            retry = 1;
        }
        break;
    default:
        print("init: rc scan error — bare boot\n");
        break;
    }

    proc_self_test();

    if (retry) {
        print("init: re-scanning /etc/rc every ");
        print_dec(RC_RETRY_SECS);
        print("s\n");
        for (;;) {
            bsd_sleep(RC_RETRY_SECS);
            int n = rc_load();
            if (n > 0) {
                print("init: rc units appeared — booting\n");
                rt_build(n);
                service_boot();
            }
        }
    }
    idle_loop();
    __builtin_unreachable();
}


__attribute__((noreturn))
void _start(void) {
    print("init: starting (pid=");
    print_dec(bsd_getpid());
    print(")\n");

    int n = rc_load();
    if (n > 0) {
        rt_build(n);
        service_boot();
    }
    bare_boot(n);
    __builtin_unreachable();
}
