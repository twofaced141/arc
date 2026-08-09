#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/signal.h"
#include "bsd/arch.h"
#include "thread.h"
#include "debug.h"
#include "vmm.h"
#include "string.h"

#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))
#define ARG3(r) ((uint64_t)bsd_syscall_arg2(r))

int64_t sys_signal(proc_t *p, registers_t *r) {
    int sig = (int)ARG1(r);
    sighandler_t handler = (sighandler_t)ARG2(r);

    if (!signal_is_valid(sig) || sig == SIGKILL || sig == SIGSTOP)
        return -1;

    sighandler_t old = p->signals.handler[sig];
    p->signals.handler[sig] = handler;

    /* Clear pending */
    p->signals.pending[sig] = 0;

    return (int64_t)(uintptr_t)old;
}

/* POSIX kill permission: the caller may signal the target if it is
 * privileged (euid 0) or its effective uid matches the target's real
 * or effective uid.  A process always has permission to signal itself. */
static int kill_permitted(proc_t *sender, proc_t *target) {
    if (sender == target)
        return 1;
    if (sender->euid == 0)
        return 1;
    return (sender->euid == target->uid || sender->euid == target->euid);
}

/* Deliver one signal to one process.  Called with no table locks held. */
static void kill_deliver(proc_t *target, int sig, proc_t *sender,
                         registers_t *r) {
    /* Set pending */
    target->signals.pending[sig] = 1;

    /* Stop/continue signals update the wait-visible state */
    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        target->stopped = 1;
        target->exit_sig = (uint8_t)sig;
        target->state = PRS_STOPPED;
        proc_t *parent = proc_find(target->ppid);
        if (parent)
            waitq_wake_all(&parent->waitq);
    } else if (sig == SIGCONT) {
        target->stopped = 0;
        target->exit_sig = 0;
        if (target->state == PRS_STOPPED)
            target->state = PRS_NORMAL;
    }

    /* SIGKILL is immediate for the caller itself; for other processes
     * it is delivered at the target's next entry to the kernel (the
     * pending flag can never be cleared or blocked, and SIG_DFL on
     * SIGKILL terminates). */
    if (sig == SIGKILL) {
        target->exit_sig = SIGKILL;
        target->exit_status = 9;
        if (target == sender) {
            proc_exit(9, r);
            thread_exit(9); /* never returns */
        }
    }

    /* Wake the target so a blocked syscall returns -EINTR */
    if (target != sender) {
        proc_wakeup(target);
        waitq_wake_all(&target->waitq);
    }
}

int64_t sys_kill(proc_t *p, registers_t *r) {
    pid_t pid = (pid_t)ARG1(r);
    int sig   = (int)ARG2(r);

    if (!signal_is_valid(sig) && sig != 0)
        return -EINVAL;

    if (pid > 0) {
        proc_t *target = proc_find(pid);
        if (!target) return -ESRCH;
        if (!kill_permitted(p, target))
            return -EPERM;
        if (sig != 0)
            kill_deliver(target, sig, p, r);
        return 0;
    }

    /* pid == 0: own process group; pid == -1: all processes;
     * pid < -1: process group -pid.  The caller may signal exactly the
     * targets it has permission for. */
    pid_t targets[256];
    int n = proc_collect_kill_targets(pid, p->pgrp, targets,
                                      (int)(sizeof(targets) / sizeof(targets[0])));
    if (n == 0)
        return -ESRCH;

    int delivered = 0;
    for (int i = 0; i < n; i++) {
        proc_t *target = proc_find(targets[i]);
        if (!target)
            continue; /* exited between collect and deliver */
        if (!kill_permitted(p, target))
            continue;
        if (sig != 0)
            kill_deliver(target, sig, p, r);
        delivered = 1;
    }

    if (!delivered)
        return -EPERM;
    return 0;
}

int64_t sys_sigaction(proc_t *p, registers_t *r) {
    int sig = (int)ARG1(r);
    const sigaction_t *act   = (const sigaction_t *)ARG2(r);
    sigaction_t *oldact      = (sigaction_t *)ARG3(r);

    if (!signal_is_valid(sig) || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;

    /* Return old action */
    if (oldact) {
        sigaction_t old;
        old.sa_handler = p->signals.handler[sig];
        old.sa_mask = p->signals.sa_mask[sig];
        old.sa_flags = (int)p->signals.sa_flags[sig];
        old.sa_restorer = (void *)p->signals.sa_restorer[sig];
        copy_to_user(oldact, &old, sizeof(sigaction_t));
    }

    /* Set new action */
    if (act) {
        sigaction_t newact;
        if (copy_from_user(&newact, act, sizeof(sigaction_t)) != 0)
            return -EFAULT;
        p->signals.handler[sig] = newact.sa_handler;
        p->signals.sa_mask[sig] = newact.sa_mask;
        p->signals.sa_flags[sig] = (uint32_t)newact.sa_flags;
        p->signals.sa_restorer[sig] = (uintptr_t)newact.sa_restorer;
    }

    /* Clear pending on setup */
    p->signals.pending[sig] = 0;

    return 0;
}

/* Suspend until a signal is delivered: temporarily set the blocked
 * mask, then wait.  Always returns -EINTR (the pending signal will be
 * handled when we return to userspace). */
int64_t sys_sigsuspend(proc_t *p, registers_t *r) {
    uint32_t mask = (uint32_t)ARG1(r);

    uint32_t old[NSIG];
    memcpy(old, p->signals.blocked, sizeof(old));

    memset(p->signals.blocked, 0, sizeof(p->signals.blocked));
    for (int i = 0; i < NSIG; i++)
        if (mask & (1u << i))
            p->signals.blocked[i] = 1;

    if (!signal_has_pending(p))
        waitq_sleep(&p->waitq);

    memcpy(p->signals.blocked, old, sizeof(old));
    return -EINTR;
}

int64_t sys_sigaltstack(proc_t *p, registers_t *r) {
    const stack_t *uss = (const stack_t *)ARG1(r);
    stack_t *uoss      = (stack_t *)ARG2(r);

    if (uoss) {
        stack_t oss;
        oss.ss_sp = (void *)p->signals.ss_sp;
        oss.ss_size = p->signals.ss_size;
        if (p->signals.on_altstack)
            oss.ss_flags = SS_ONSTACK;
        else if (!p->signals.ss_active)
            oss.ss_flags = SS_DISABLE;
        else
            oss.ss_flags = 0;
        copy_to_user(uoss, &oss, sizeof(stack_t));
    }

    if (uss) {
        stack_t ss;
        if (copy_from_user(&ss, uss, sizeof(stack_t)) != 0)
            return -EFAULT;

        if (p->signals.on_altstack)
            return -EPERM;

        if (ss.ss_flags & SS_DISABLE) {
            p->signals.ss_sp = 0;
            p->signals.ss_size = 0;
            p->signals.ss_active = 0;
            return 0;
        }
        if (ss.ss_flags != 0)
            return -EINVAL;
        if (ss.ss_size < MINSIGSTKSZ)
            return -ENOMEM;

        p->signals.ss_sp = (uintptr_t)ss.ss_sp;
        p->signals.ss_size = ss.ss_size;
        p->signals.ss_active = 1;
    }
    return 0;
}

/* Restore the blocked mask saved in the sigframe (POSIX: sigreturn
 * restores the mask that was in effect when the signal arrived). */
static void sigreturn_restore_mask(proc_t *p, uint32_t mask) {
    memset(p->signals.blocked, 0, sizeof(p->signals.blocked));
    for (int i = 1; i < NSIG; i++)
        if (mask & (1u << i))
            p->signals.blocked[i] = 1;
}

/* Leave signal-handling state: the interrupted context is resumed from
 * the frame.  sigreturn never returns to the caller. */
static void sigreturn_done(proc_t *p) {
    p->signals.in_signal = 0;
    p->signals.on_altstack = 0;
    p->signals.sigframe_addr = 0;
}

#if defined(__i386__)
int64_t sys_sigreturn(proc_t *p, registers_t *r) {
    sigframe_t frame;
    void *frame_addr = (void *)p->signals.sigframe_addr;
    if (!frame_addr)
        frame_addr = (void *)r->useresp;
    if (copy_from_user(&frame, frame_addr, sizeof(sigframe_t)) != 0)
        return -EFAULT;

    /* SA_RESTART: the interrupted syscall is re-executed — restore the
     * argument registers, put the syscall number back into eax and let
     * the rewound PC re-run `int $0x80`. */
    if (frame.restart_active) {
        r->ebx = frame.restart_arg0;
        r->ecx = frame.restart_arg1;
        r->edx = frame.restart_arg2;
        r->esi = frame.restart_arg3;
        r->edi = frame.restart_arg4;
        r->ebp = frame.restart_arg5;
        p->signals.restart_sysno = (int)frame.restart_sysno;
        sigreturn_restore_mask(p, frame.saved_mask);
        sigreturn_done(p);
        return (int64_t)frame.restart_sysno + 1024;
    }

    r->eip     = frame.saved_eip;
    r->cs      = frame.saved_cs;
    r->eflags  = frame.saved_eflags;
    r->useresp = frame.saved_esp;
    r->ss      = frame.saved_ss;

    sigreturn_restore_mask(p, frame.saved_mask);
    sigreturn_done(p);
    return 0;
}
#elif defined(__x86_64__)
int64_t sys_sigreturn(proc_t *p, registers_t *r) {
    sigframe_t frame;
    void *frame_addr = (void *)p->signals.sigframe_addr;
    if (!frame_addr)
        frame_addr = (void *)r->rsp;
    if (copy_from_user(&frame, frame_addr, sizeof(sigframe_t)) != 0)
        return -EFAULT;

    r->rip    = frame.saved_rip;
    r->cs     = frame.saved_cs;
    r->rflags = frame.saved_rflags;
    r->rsp    = frame.saved_rsp;
    r->ss     = frame.saved_ss;

    /* SA_RESTART: the interrupted syscall is re-executed — restore the
     * argument registers (the syscall instruction will fire again with
     * the syscall number in rax; the PC was rewound at delivery). */
    if (frame.restart_active) {
        r->rdi = frame.restart_arg0;
        r->rsi = frame.restart_arg1;
        r->rdx = frame.restart_arg2;
        r->r10 = frame.restart_arg3;
        r->r8  = frame.restart_arg4;
        r->r9  = frame.restart_arg5;
        p->signals.restart_sysno = (int)frame.restart_sysno;
        sigreturn_restore_mask(p, frame.saved_mask);
        sigreturn_done(p);
        /* Dispatch writes this value into rax, re-arming the syscall. */
        return (int64_t)frame.restart_sysno + 1024;
    }

    r->rax    = frame.saved_rax;

    sigreturn_restore_mask(p, frame.saved_mask);
    sigreturn_done(p);
    /* Never returns to the caller: the stub writes this into RAX of the
     * restored frame before iretq, so the interrupted syscall's result
     * (e.g. -EINTR) reaches userspace. */
    return (int64_t)frame.saved_rax;
}
#else /* arm64 */
int64_t sys_sigreturn(proc_t *p, registers_t *r) {
    sigframe_t frame;
    void *frame_addr = (void *)p->signals.sigframe_addr;
    if (!frame_addr)
        frame_addr = (void *)r->sp;
    if (copy_from_user(&frame, frame_addr, sizeof(sigframe_t)) != 0)
        return -EFAULT;

    r->lr      = frame.saved_lr;
    r->spsr    = frame.saved_spsr;
    r->elr     = frame.saved_elr;
    r->sp      = frame.saved_sp;

    /* SA_RESTART: restore the argument registers and put the syscall
     * number back into x0; the rewound elr re-runs the svc. */
    if (frame.restart_active) {
        r->x[1] = frame.restart_arg0;
        r->x[2] = frame.restart_arg1;
        r->x[3] = frame.restart_arg2;
        r->x[4] = frame.restart_arg3;
        r->x[5] = frame.restart_arg4;
        r->x[6] = frame.restart_arg5;
        p->signals.restart_sysno = (int)frame.restart_sysno;
        sigreturn_restore_mask(p, frame.saved_mask);
        sigreturn_done(p);
        /* Dispatch writes this value into x0, re-arming the syscall. */
        return (int64_t)frame.restart_sysno + 1024;
    }

    r->x[0]    = frame.saved_x0;

    sigreturn_restore_mask(p, frame.saved_mask);
    sigreturn_done(p);
    /* Never returns to the caller: the stub writes this into x0 of the
     * restored frame before eret. */
    return (int64_t)frame.saved_x0;
}
#endif

int64_t sys_sigprocmask(proc_t *p, registers_t *r) {
    int how = (int)ARG1(r);
    const uint32_t *set    = (const uint32_t *)ARG2(r);
    uint32_t *oldset       = (uint32_t *)ARG3(r);

    /* Return old mask (packed: bit i = blocked[i], i in 1..31) */
    if (oldset) {
        uint32_t oldmask = 0;
        for (int i = 1; i < NSIG; i++)
            if (p->signals.blocked[i])
                oldmask |= (1u << i);
        copy_to_user(oldset, &oldmask, sizeof(uint32_t));
    }

    /* Apply new mask */
    if (set) {
        uint32_t newmask;
        if (copy_from_user(&newmask, set, sizeof(uint32_t)) != 0)
            return -EFAULT;

        switch (how) {
        case SIG_BLOCK:
            for (int i = 0; i < NSIG; i++)
                if (newmask & (1u << i))
                    p->signals.blocked[i] = 1;
            break;
        case SIG_UNBLOCK:
            for (int i = 0; i < NSIG; i++)
                if (newmask & (1u << i))
                    p->signals.blocked[i] = 0;
            break;
        case SIG_SETMASK:
            memset(p->signals.blocked, 0, sizeof(p->signals.blocked));
            for (int i = 0; i < NSIG; i++)
                if (newmask & (1u << i))
                    p->signals.blocked[i] = 1;
            break;
        default:
            return -EINVAL;
        }
    }

    return 0;
}

