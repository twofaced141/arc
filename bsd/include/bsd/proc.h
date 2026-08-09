#ifndef BSD_PROC_H
#define BSD_PROC_H

#include <stdint.h>
#include <stddef.h>
#include "thread.h"
#include "vmm.h"
#include "spinlock.h"
#include "mman.h"

typedef int32_t pid_t;

/* Process states */
#define PRS_NORMAL   1
#define PRS_ZOMBIE   2
#define PRS_STOPPED  3
#define PRS_SLEEP    4
#define PRS_RUNNING  5

/* clone(2) flags (Linux-compatible values) */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* Thread group: share address space + page_dir with the group leader. */
#define PRS_THREAD (1 << 16)

/* Max processes */
#define PROC_MAX  1024
#define PROC_NULL 0

/* File descriptor limits */
#define FD_MAX    256
#define FD_INITIAL 16
#define FD_CLOEXEC (1 << 0)

/* Resource limits (getrlimit/setrlimit, per-process; values stored in
 * proc_t.rlim[], indexed by RLIMIT_*). */
#define RLIMIT_CPU      0
#define RLIMIT_FSIZE    1
#define RLIMIT_DATA     2
#define RLIMIT_STACK    3
#define RLIMIT_CORE     4
#define RLIMIT_NOFILE   7
#define RLIMIT_AS       9
#define RLIM_NLIMITS    16
#define RLIM_INFINITY   (~0ULL)

typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} rlimit_t;

/* Default soft/hard limits installed at proc_alloc(). */
void rlimit_init_defaults(rlimit_t *rlim);

/* Per-process file descriptor */
typedef struct {
    int      fd;
    void    *vnode_ptr;   /* opaque vnode pointer */
    int      flags;
    int64_t  offset;
    int      mode;
    uint32_t cloexec : 1;
    uint32_t used    : 1;
} filedesc_t;

/* Signal state per process */
#define NSIG 32
typedef void (*sighandler_t)(int);

typedef struct {
    sighandler_t handler[NSIG];
    uint32_t     pending[NSIG];
    uint32_t     blocked[NSIG];
    int          in_signal;     /* currently delivering a signal */

    /* Per-signal sigaction data */
    uint32_t     sa_mask[NSIG];   /* mask applied while handler runs */
    uint32_t     sa_flags[NSIG];
    uintptr_t    sa_restorer[NSIG]; /* address returned to after the handler */

    /* sigaltstack */
    uintptr_t    ss_sp;
    size_t       ss_size;
    uint32_t     ss_active;       /* stack installed (SS_ONSTACK) */
    uint32_t     on_altstack;     /* currently running on it */

    /* SA_RESTART machinery: when an interruptible syscall is aborted by
     * a signal whose handler has SA_RESTART, the sigframe saves the
     * syscall number and sigreturn rewinds the PC so the syscall
     * instruction is re-executed (POSIX restart). */
    int          syscall_restartable;  /* current syscall may restart */
    int          restart_sysno;        /* syscall number to re-run */
    int          restart_frame;        /* frame was built for restart */

    /* sigreturn frame: address of the sigframe sigreturn must pop.
     * The interrupted context itself lives inside the frame. */
    uintptr_t    sigframe_addr;
#ifdef __x86_64__
    uint64_t     saved_rip;     /* for sigreturn */
    uint64_t     saved_rsp;
#else
    uint32_t     saved_eip;     /* for sigreturn */
    uint32_t     saved_esp;
#endif
} sigstate_t;

/* TTY info */
typedef struct {
    int    tty_dev;
    pid_t  pgrp;      /* process group */
    pid_t  session;   /* session leader */
} ttyinfo_t;

/* Wait queue for blocking syscalls.  Sleepers are linked through
 * proc->wait_next; wakers remove and unblock them. */
typedef struct waitq {
    struct proc *head;
    spinlock_t   lock;
} waitq_t;

/* Process control block */
typedef struct proc {
    pid_t   pid;
    pid_t   ppid;
    pid_t   tgid;      /* thread group id (== pid for the group leader) */
    pid_t   pgrp;
    pid_t   session;
    int     state;
    int     exit_status;

    /* Thread-group state (clone CLONE_THREAD) */
    uintptr_t tls_base;         /* CLONE_SETTLS per-thread TLS base */
    uintptr_t clear_child_tid; /* CLONE_CHILD_CLEARTID: user pid to zero on exit */
    uintptr_t set_child_tid;   /* CLONE_CHILD_SETTID: user pid written at creation */
    int     is_thread;         /* clone CLONE_THREAD member (shares page_dir) */

    /* Exit status for waitpid: exit_sig != 0 means the process was
     * killed by a signal (or stopped, for stopped children). */
    uint8_t exit_sig;
    uint8_t stopped;

    /* Credentials */
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;
    uint32_t umask;

    /* Resource limits (indexed by RLIMIT_*) */
    rlimit_t rlim[RLIM_NLIMITS];
    
    char    name[32];
    
    /* MK thread backing this process */
    thread_t *thread;
    
    /* Address space */
    page_directory_t *page_dir;
    
    /* User-level register save area (for syscall return) */
    uint32_t user_eip;
    uint32_t user_esp;
    uint32_t user_eflags;
    
    /* File descriptors: dynamically allocated table, grown on demand
     * from FD_INITIAL up to FD_MAX slots (see proc.c). */
    filedesc_t *fds;
    int         fd_capacity;
    spinlock_t fd_lock;
    
    /* Signal state */
    sigstate_t signals;
    
    /* TTY */
    ttyinfo_t tty;
    
    /* Resource usage */
    uint32_t  ticks;
    
    /* Scheduling */
    int   priority;
    int   nice;
    
    /* MMAP allocation cursor (for phys_map / dma_alloc auto-placement) */
    uint32_t mmap_next;

    /* Heap (brk/sbrk) — pointer to end of heap, grows upward from USER_HEAP_START */
    uint64_t heap_end;

    /* mmap (bsd/sys/sys_mmap.c): allocation cursor grows upward from
     * USER_MMAP_START; regions track mapped ranges for munmap/mprotect
     * and lazy page-in on fault. */
    uintptr_t mmap_cursor;
    struct mmap_region mmap_regions[MMAP_MAX_REGIONS];

    /* Current working directory */
#define CWD_MAX 256
    char cwd[CWD_MAX];

    /* Linked list */
    struct proc *next;
    struct proc *children;
    struct proc *sibling;

    /* Wait-queue linkage (blocking syscalls) */
    struct proc *wait_next;
    waitq_t      waitq;     /* waitpid waits here; proc_exit wakes it */

    /* Futex queue this process is currently counted on (bsd/uipc/futex.c).
     * NULL when not sleeping in a futex; requeue moves the count together
     * with the process. */
    struct futex_q *futex_q;
} proc_t;

/* Process table access */
proc_t *proc_alloc(pid_t ppid);
void    proc_free(proc_t *p);
proc_t *proc_find(pid_t pid);
proc_t *proc_current(void);
pid_t   proc_alloc_pid(void);
void    proc_free_pid(pid_t pid);
int     proc_count(void);
int     proc_collect_kill_targets(pid_t pid, pid_t caller_pgrp,
                                  pid_t *out, int max);

/* Process lifecycle */
void    proc_init(void);
int     proc_fork(registers_t *r);
int     proc_clone(registers_t *r, unsigned long flags, uintptr_t child_stack,
                   uintptr_t parent_tid, uintptr_t tls, uintptr_t child_tid);
int     proc_execve(registers_t *r);
void    proc_exit(int exitcode, registers_t *r);
void    proc_thread_exit(int exitcode);  /* exit a clone thread (group member) */
void    proc_kill_by_signal(int sig, registers_t *r);
pid_t   proc_waitpid(pid_t pid, int *status, int options);

/* waitpid(2) options */
#define WNOHANG   1
#define WUNTRACED 2

/* wait status encoding (Linux/BSD compatible) */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)

/* Process file descriptor operations */
int     proc_fd_alloc(proc_t *p);
int     proc_fd_dealloc(proc_t *p, int fd);
filedesc_t *proc_fd_get(proc_t *p, int fd);
int     proc_fd_dup(proc_t *dst, proc_t *src, int fd);
int     proc_fd_dup2(proc_t *dst, proc_t *src, int oldfd, int newfd);

/* Wake a process blocked in a syscall (used by signal delivery) */
void    proc_wakeup(proc_t *p);

/* Wait queue API (interruptible blocking) */
void    waitq_init(waitq_t *wq);
int     waitq_sleep(waitq_t *wq);   /* -EINTR when a signal is pending */
int     waitq_sleep_timeout(waitq_t *wq, uint64_t deadline);
/* Like waitq_sleep_timeout, but the caller has already linked this
 * process into wq (while holding the lock that wakers use); the queue
 * link is not repeated here. */
int     waitq_sleep_timeout_linked(waitq_t *wq, uint64_t deadline);
void    waitq_wake_all(waitq_t *wq);
void    waitq_wake_one(waitq_t *wq);

/* Kernel threads (BSD services) */
typedef void (*kthread_func_t)(void *);
thread_t *kthread_create(kthread_func_t func, void *arg, const char *name);

#endif
