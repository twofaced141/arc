#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include "isr.h"

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

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define SA_NOCLDSTOP 1

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction {
    void (*sa_handler)(int);
    uint32_t sa_flags;
};
typedef struct sigaction sigaction_t;

void deliver_signal(registers_t *r);
int sys_sigaction(int signum, const sigaction_t *act, sigaction_t *oldact);
int sys_sigreturn(registers_t *r);
int sys_kill_sig(int pid, int signum);
int sys_sigprocmask(int how, const uint32_t *set, uint32_t *oldset);
int sys_sigpending(uint32_t *set);
int sys_sigsuspend(registers_t *r, const uint32_t *mask);
int sys_kill_pgid(int pgid, int signum);
int scheduler_check_pending_signals(void);

extern uint32_t foreground_pgid;

/* User-space sigaction wrapper */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#endif
