#ifndef BSD_SIGNAL_H
#define BSD_SIGNAL_H

#include <stdint.h>
#include "proc.h"

/* Standard signals (POSIX subset) */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

/* Signal actions */
#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

/* sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sigaction sa_flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* sigaltstack */
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define MINSIGSTKSZ  2048
#define SIGSTKSZ     8192

/* Signal stack frame layout (for delivery to userspace) */
#if defined(__x86_64__)
typedef struct {
    uint32_t signum;
    uint32_t _pad;
    uint64_t saved_rip;
    uint64_t saved_cs;
    uint64_t saved_rflags;
    uint64_t saved_rsp;
    uint64_t saved_ss;
    uint64_t saved_rax;    /* syscall result interrupted by the signal */
    uint32_t saved_mask;   /* blocked mask to restore on sigreturn */
    uint32_t _pad2;
    /* SA_RESTART: an interruptible syscall was aborted by this signal.
     * saved_rip is already rewound past the syscall instruction; the
     * argument registers below let sigreturn re-execute it verbatim. */
    uint32_t restart_active; /* frame built for a restartable syscall */
    uint32_t restart_sysno;  /* syscall number (without the +1024 offset) */
    uint64_t restart_arg0;
    uint64_t restart_arg1;
    uint64_t restart_arg2;
    uint64_t restart_arg3;
    uint64_t restart_arg4;
    uint64_t restart_arg5;
} sigframe_t;
#elif defined(__i386__) || defined(__i686__)
typedef struct {
    uint32_t signum;
    uint32_t saved_eip;
    uint32_t saved_cs;
    uint32_t saved_eflags;
    uint32_t saved_esp;
    uint32_t saved_ss;
    uint32_t saved_mask;
    /* SA_RESTART: same layout as above, 32-bit argument registers. */
    uint32_t restart_active;
    uint32_t restart_sysno;
    uint32_t restart_arg0;
    uint32_t restart_arg1;
    uint32_t restart_arg2;
    uint32_t restart_arg3;
    uint32_t restart_arg4;
    uint32_t restart_arg5;
    /* sigreturn pops this frame */
} sigframe_t;
#elif defined(__aarch64__)
typedef struct {
    uint32_t signum;
    uint32_t _pad;
    uint64_t saved_x0;
    uint64_t saved_lr;
    uint64_t saved_spsr;
    uint64_t saved_elr;
    uint64_t saved_sp;
    uint32_t saved_mask;   /* blocked mask to restore on sigreturn */
    uint32_t _pad2;
    /* SA_RESTART: elr is rewound past the `svc` instruction; x1..x6
     * hold the syscall arguments for sigreturn to restore. */
    uint32_t restart_active;
    uint32_t restart_sysno;
    uint64_t restart_arg0;
    uint64_t restart_arg1;
    uint64_t restart_arg2;
    uint64_t restart_arg3;
    uint64_t restart_arg4;
    uint64_t restart_arg5;
} sigframe_t;
#endif

/* sigaction structure (userspace-facing) */
typedef struct {
    sighandler_t sa_handler;
    uint32_t     sa_mask;
    int          sa_flags;
    void        (*sa_restorer)(void);
} sigaction_t;

/* sigaltstack structure (userspace-facing) */
typedef struct {
    void     *ss_sp;
    int       ss_flags;
    size_t    ss_size;
} stack_t;

/* Signal layer API */
void signal_init(void);

/* Deliver a signal to a process */
int  signal_deliver(proc_t *p, int sig, registers_t *r);

/* Check for pending signals on return to userspace */
int  signal_check_pending(proc_t *p, registers_t *r);

/* Default signal action */
typedef enum {
    SIGACT_TERM,    /* terminate */
    SIGACT_CORE,    /* terminate with core dump */
    SIGACT_IGN,     /* ignore */
    SIGACT_STOP,    /* stop */
    SIGACT_CONT,    /* continue if stopped */
} sigaction_default_t;

sigaction_default_t signal_default_action(int sig);
int  signal_is_valid(int sig);
int  signal_has_pending(proc_t *p);

#endif
