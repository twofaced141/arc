#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>
#include <stdint.h>

typedef int sig_atomic_t;
typedef uint32_t sigset_t;

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
#define SIGUNUSED 31

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define CLD_EXITED   1
#define CLD_KILLED   2
#define CLD_DUMPED   3
#define CLD_TRAPPED  4
#define CLD_STOPPED  5
#define CLD_CONTINUED 6

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define NSIG 32

struct sigevent {
    int sigev_signo;
    int sigev_notify;
};

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int si_status;
} siginfo_t;

/* Layout matches kernel: handler(4) + flags(4) + pad(8). */
struct sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    sigset_t sa_mask;
    void (*sa_restorer)(void);
};

void (*signal(int signum, void (*handler)(int)))(int);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

static inline int sigemptyset(sigset_t *set) { *set = 0; return 0; }
static inline int sigfillset(sigset_t *set) { *set = ~(sigset_t)0; return 0; }
static inline int sigaddset(sigset_t *set, int signum) { if (signum < 1 || signum >= 32) return -1; *set |= (1U << (signum - 1)); return 0; }
static inline int sigdelset(sigset_t *set, int signum) { if (signum < 1 || signum >= 32) return -1; *set &= ~(1U << (signum - 1)); return 0; }
static inline int sigismember(const sigset_t *set, int signum) { if (signum < 1 || signum >= 32) return 0; return (*set >> (signum - 1)) & 1; }

#endif
