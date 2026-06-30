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

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define SA_NOCLDSTOP 1

typedef struct {
    void (*sa_handler)(int);
    uint32_t sa_flags;
} sigaction_t;

void deliver_signal(registers_t *r);
int sys_sigaction(int signum, const sigaction_t *act, sigaction_t *oldact);
int sys_sigreturn(registers_t *r);
int sys_kill_sig(int pid, int signum);

#endif
