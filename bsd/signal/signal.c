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


#include "bsd/signal.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/arch.h"
#include "thread.h"
#include "debug.h"
#include "string.h"
#include "vmm.h"

void signal_init(void) {
    log_print(LOG_LEVEL_DEBUG, "signal: init\r\n");
}

int signal_is_valid(int sig) {
    return (sig >= 1 && sig < NSIG) ? 1 : 0;
}

int signal_has_pending(proc_t *p) {
    if (!p)
        return 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (p->signals.pending[sig] && !p->signals.blocked[sig])
            return 1;
    }
    return 0;
}

sigaction_default_t signal_default_action(int sig) {
    switch (sig) {
    case SIGCHLD: case SIGURG: case SIGWINCH:
        return SIGACT_IGN;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return SIGACT_STOP;
    case SIGCONT:
        return SIGACT_CONT;
    case SIGQUIT: case SIGILL: case SIGTRAP: case SIGABRT:
    case SIGFPE: case SIGSEGV: case SIGBUS:
        return SIGACT_CORE;
    default:
        return SIGACT_TERM;
    }
}

#if defined(__i386__)

int signal_deliver(proc_t *p, int sig, registers_t *r) {
    if (!p || !signal_is_valid(sig))
        return -EINVAL;

    if (p->signals.blocked[sig]) {
        p->signals.pending[sig] = 1;
        return 0;
    }

    sighandler_t h = p->signals.handler[sig];

    if (h == SIG_IGN) {
        p->signals.pending[sig] = 0;
        return 0;
    }

    if (h == SIG_DFL) {
        switch (signal_default_action(sig)) {
        case SIGACT_IGN:
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_STOP:
            p->state = PRS_STOPPED;
            p->stopped = 1;
            p->exit_sig = (uint8_t)sig;
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_CONT:
            p->state = PRS_NORMAL;
            p->stopped = 0;
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_TERM:
        case SIGACT_CORE:
            p->exit_sig = (uint8_t)sig;
            proc_exit(0, r);
            /* The process is gone — never return to the syscall/iret
             * path of a dead process.  Switch to the next thread. */
            thread_exit(sig);
            return 0; /* unreachable */
        }
    }

    sigframe_t frame;
    frame.signum     = sig;
    frame.saved_eip  = r->eip;
    frame.saved_cs   = r->cs;
    frame.saved_eflags = r->eflags;
    frame.saved_esp  = r->useresp;
    frame.saved_ss   = r->ss;

    /* SA_RESTART: an interruptible syscall was aborted by this signal.
     * Rewind the PC past the syscall instruction and save the syscall
     * number + argument registers; sigreturn re-executes it after the
     * handler returns.  A fresh frame starts restart-inactive. */
    uint32_t new_eip = r->eip;
    if (p->signals.syscall_restartable &&
        (p->signals.sa_flags[sig] & SA_RESTART)) {
        frame.restart_active = 1;
        frame.restart_sysno  = (uint32_t)p->signals.restart_sysno;
        frame.restart_arg0   = r->ebx;
        frame.restart_arg1   = r->ecx;
        frame.restart_arg2   = r->edx;
        frame.restart_arg3   = r->esi;
        frame.restart_arg4   = r->edi;
        frame.restart_arg5   = r->ebp;
        new_eip = r->eip - BSD_SYSCALL_INS_LEN;
        frame.saved_eip = new_eip;
    } else {
        frame.restart_active = 0;
        frame.restart_sysno  = 0;
    }

    /* Compute the handler mask but do NOT commit it yet: the frame is
     * installed in user memory first, and only on success does any
     * kernel state change.  A failed copy (bad altstack, unmapped
     * stack) must leave the signal pending and the mask untouched —
     * otherwise the signal silently disappears and sigreturn never
     * runs to restore the saved mask. */
    uint32_t old_mask = 0;
    for (int i = 1; i < NSIG; i++)
        if (p->signals.blocked[i])
            old_mask |= (1u << i);
    frame.saved_mask = old_mask;

    uint32_t new_mask = old_mask;
    if (!(p->signals.sa_flags[sig] & SA_NODEFER))
        new_mask |= (1u << sig);
    new_mask |= p->signals.sa_mask[sig];

    uint32_t stack = r->useresp;
    if ((p->signals.sa_flags[sig] & SA_ONSTACK) && p->signals.ss_active &&
        !p->signals.on_altstack)
        stack = (uint32_t)(p->signals.ss_sp + p->signals.ss_size);

    /* Layout: the handler's entry rsp points at the restorer (its
     * return address); the sigframe sits right below it, so nothing
     * the handler pushes can clobber it.  `ret` pops the restorer,
     * which invokes sigreturn. */
    uint32_t restorer = (uint32_t)p->signals.sa_restorer[sig];
    uint32_t frame_addr, esp;
    if (restorer) {
        /* Handler sees: [esp]   = restorer (return address for `ret`)
         *               [esp+4] = sig (cdecl first argument)
         *               frame sits below */
        frame_addr = stack - 8 - sizeof(sigframe_t);
        esp = stack - 8;
    } else {
        frame_addr = stack - sizeof(sigframe_t);
        esp = frame_addr;
    }
    if (copy_to_user((void *)(uintptr_t)frame_addr, &frame, sizeof(sigframe_t)) != 0)
        return -EFAULT;
    if (restorer &&
        copy_to_user((void *)(uintptr_t)esp, &restorer, sizeof(restorer)) != 0)
        return -EFAULT;
    if (restorer &&
        copy_to_user((void *)(uintptr_t)(esp + 4), &sig, sizeof(sig)) != 0)
        return -EFAULT;

    /* Frame is in user memory — commit all kernel-side state now. */
    p->signals.pending[sig] = 0;
    for (int i = 1; i < NSIG; i++)
        p->signals.blocked[i] = (new_mask & (1u << i)) ? 1 : 0;
    if (frame.restart_active)
        p->signals.restart_frame = 1;

    r->useresp = esp;
    r->eip = (uint32_t)h;
    p->signals.in_signal = 1;
    p->signals.on_altstack = (frame_addr >= p->signals.ss_sp &&
                              frame_addr < p->signals.ss_sp + p->signals.ss_size);
    p->signals.sigframe_addr = frame_addr;

    return 0;
}

#elif defined(__x86_64__)

int signal_deliver(proc_t *p, int sig, registers_t *r) {
    if (!p || !signal_is_valid(sig))
        return -EINVAL;

    if (p->signals.blocked[sig]) {
        p->signals.pending[sig] = 1;
        return 0;
    }

    sighandler_t h = p->signals.handler[sig];

    if (h == SIG_IGN) {
        p->signals.pending[sig] = 0;
        return 0;
    }

    if (h == SIG_DFL) {
        switch (signal_default_action(sig)) {
        case SIGACT_IGN:
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_STOP:
            p->state = PRS_STOPPED;
            p->stopped = 1;
            p->exit_sig = (uint8_t)sig;
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_CONT:
            p->state = PRS_NORMAL;
            p->stopped = 0;
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_TERM:
        case SIGACT_CORE:
            p->exit_sig = (uint8_t)sig;
            proc_exit(0, r);
            /* The process is gone — never return to the syscall/iret
             * path of a dead process.  Switch to the next thread. */
            thread_exit(sig);
            return 0; /* unreachable */
        }
    }

    sigframe_t frame;
    frame.signum     = sig;
    frame._pad       = 0;
    frame._pad2      = 0;
    frame.saved_rip  = r->rip;
    frame.saved_cs   = r->cs;
    frame.saved_rflags = r->rflags;
    frame.saved_rsp  = r->rsp;
    frame.saved_ss   = r->ss;
    frame.saved_rax  = r->rax;

    uint64_t new_rip = r->rip;
    if (p->signals.syscall_restartable &&
        (p->signals.sa_flags[sig] & SA_RESTART)) {
        frame.restart_active = 1;
        frame.restart_sysno  = (uint32_t)p->signals.restart_sysno;
        frame.restart_arg0   = r->rdi;
        frame.restart_arg1   = r->rsi;
        frame.restart_arg2   = r->rdx;
        frame.restart_arg3   = r->r10;
        frame.restart_arg4   = r->r8;
        frame.restart_arg5   = r->r9;
        new_rip = r->rip - BSD_SYSCALL_INS_LEN;
        frame.saved_rip = new_rip;
    } else {
        frame.restart_active = 0;
        frame.restart_sysno  = 0;
    }

    /* Compute the handler mask but do NOT commit it yet: the frame is
     * installed in user memory first, and only on success does any
     * kernel state change.  A failed copy (bad altstack, unmapped
     * stack) must leave the signal pending and the mask untouched —
     * otherwise the signal silently disappears and sigreturn never
     * runs to restore the saved mask. */
    uint32_t old_mask = 0;
    for (int i = 1; i < NSIG; i++)
        if (p->signals.blocked[i])
            old_mask |= (1u << i);
    frame.saved_mask = old_mask;

    uint32_t new_mask = old_mask;
    if (!(p->signals.sa_flags[sig] & SA_NODEFER))
        new_mask |= (1u << sig);
    new_mask |= p->signals.sa_mask[sig];

    uint64_t stack = r->rsp;
    if ((p->signals.sa_flags[sig] & SA_ONSTACK) && p->signals.ss_active &&
        !p->signals.on_altstack)
        stack = p->signals.ss_sp + p->signals.ss_size;

    /* Layout: the handler's entry rsp points at the restorer (its
     * return address); the sigframe sits right below it, so nothing
     * the handler pushes can clobber it.  `ret` pops the restorer,
     * which invokes sigreturn. */
    uint64_t restorer = p->signals.sa_restorer[sig];
    uint64_t frame_addr, rsp;
    if (restorer) {
        frame_addr = stack - 8 - sizeof(sigframe_t);
        rsp = stack - 8;
    } else {
        frame_addr = stack - sizeof(sigframe_t);
        rsp = frame_addr;
    }
    if (copy_to_user((void *)frame_addr, &frame, sizeof(sigframe_t)) != 0)
        return -EFAULT;
    if (restorer &&
        copy_to_user((void *)rsp, &restorer, sizeof(restorer)) != 0)
        return -EFAULT;

    /* Frame is in user memory — commit all kernel-side state now. */
    p->signals.pending[sig] = 0;
    for (int i = 1; i < NSIG; i++)
        p->signals.blocked[i] = (new_mask & (1u << i)) ? 1 : 0;
    if (frame.restart_active)
        p->signals.restart_frame = 1;

    r->rsp = rsp;
    r->rdi = (uint64_t)sig; /* SysV amd64: first arg in RDI */
    r->rip = (uint64_t)h;
    p->signals.in_signal = 1;
    p->signals.on_altstack = (frame_addr >= p->signals.ss_sp &&
                              frame_addr < p->signals.ss_sp + p->signals.ss_size);
    p->signals.sigframe_addr = frame_addr;

    return 0;
}

#else /* arm64 */

int signal_deliver(proc_t *p, int sig, registers_t *r) {
    if (!p || !signal_is_valid(sig))
        return -EINVAL;

    if (p->signals.blocked[sig]) {
        p->signals.pending[sig] = 1;
        return 0;
    }

    sighandler_t h = p->signals.handler[sig];

    if (h == SIG_IGN) {
        p->signals.pending[sig] = 0;
        return 0;
    }

    if (h == SIG_DFL) {
        switch (signal_default_action(sig)) {
        case SIGACT_IGN:
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_STOP:
            p->state = PRS_STOPPED;
            p->stopped = 1;
            p->exit_sig = (uint8_t)sig;
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_CONT:
            p->state = PRS_NORMAL;
            p->stopped = 0;
            p->signals.pending[sig] = 0;
            return 0;
        case SIGACT_TERM:
        case SIGACT_CORE:
            p->exit_sig = (uint8_t)sig;
            proc_exit(0, r);
            /* The process is gone — never return to the syscall/iret
             * path of a dead process.  Switch to the next thread. */
            thread_exit(sig);
            return 0; /* unreachable */
        }
    }

    sigframe_t frame;
    frame.signum     = sig;
    frame._pad       = 0;
    frame._pad2      = 0;
    frame.saved_x0   = r->x[0];
    frame.saved_lr   = r->lr;
    frame.saved_spsr = r->spsr;
    frame.saved_elr  = r->elr;
    frame.saved_sp   = r->sp;

    /* SA_RESTART: an interruptible syscall was aborted by this signal.
     * Rewind the PC past the svc instruction and save the syscall
     * number + argument registers; sigreturn re-executes it after the
     * handler returns.  A fresh frame starts restart-inactive. */
    uint64_t new_elr = r->elr;
    if (p->signals.syscall_restartable &&
        (p->signals.sa_flags[sig] & SA_RESTART)) {
        frame.restart_active = 1;
        frame.restart_sysno  = (uint32_t)p->signals.restart_sysno;
        frame.restart_arg0   = r->x[1];
        frame.restart_arg1   = r->x[2];
        frame.restart_arg2   = r->x[3];
        frame.restart_arg3   = r->x[4];
        frame.restart_arg4   = r->x[5];
        frame.restart_arg5   = r->x[6];
        new_elr = r->elr - BSD_SYSCALL_INS_LEN;
        frame.saved_elr = new_elr;
    } else {
        frame.restart_active = 0;
        frame.restart_sysno  = 0;
    }

    /* Compute the handler mask but do NOT commit it yet: the frame is
     * installed in user memory first, and only on success does any
     * kernel state change.  A failed copy (bad altstack, unmapped
     * stack) must leave the signal pending and the mask untouched —
     * otherwise the signal silently disappears and sigreturn never
     * runs to restore the saved mask. */
    uint32_t old_mask = 0;
    for (int i = 1; i < NSIG; i++)
        if (p->signals.blocked[i])
            old_mask |= (1u << i);
    frame.saved_mask = old_mask;

    uint32_t new_mask = old_mask;
    if (!(p->signals.sa_flags[sig] & SA_NODEFER))
        new_mask |= (1u << sig);
    new_mask |= p->signals.sa_mask[sig];

    uint64_t stack = r->sp;
    if ((p->signals.sa_flags[sig] & SA_ONSTACK) && p->signals.ss_active &&
        !p->signals.on_altstack)
        stack = p->signals.ss_sp + p->signals.ss_size;

    stack -= sizeof(sigframe_t);
    if (copy_to_user((void *)stack, &frame, sizeof(sigframe_t)) != 0)
        return -EFAULT;

    /* Frame is in user memory — commit all kernel-side state now. */
    p->signals.pending[sig] = 0;
    for (int i = 1; i < NSIG; i++)
        p->signals.blocked[i] = (new_mask & (1u << i)) ? 1 : 0;
    if (frame.restart_active)
        p->signals.restart_frame = 1;

    /* Handler runs with the signal number in x0 (AAPCS); it returns via
     * `ret` to the restorer in lr, which invokes sigreturn. */
    r->sp  = stack;
    r->x[0] = (uint64_t)sig;
    r->lr  = (uint64_t)p->signals.sa_restorer[sig];
    r->elr = (uint64_t)h;
    p->signals.in_signal = 1;
    p->signals.on_altstack = (stack >= p->signals.ss_sp &&
                              stack < p->signals.ss_sp + p->signals.ss_size);
    p->signals.sigframe_addr = stack;

    return 0;
}

#endif

int signal_check_pending(proc_t *p, registers_t *r) {
    if (!p) return 0;

    /* A handler is still running: the sigframe on the stack is the
     * interrupted context, delivering another signal now would overwrite
     * it.  The new signal stays pending until sigreturn pops the frame. */
    if (p->signals.in_signal)
        return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (p->signals.pending[sig] && !p->signals.blocked[sig]) {
            int ret = signal_deliver(p, sig, r);
            if (ret != 0) {
                /* The sigframe could not be installed (unmapped stack
                 * or altstack).  Retrying would spin forever and
                 * dropping the signal violates "must not be ignored":
                 * POSIX says force the default action — terminate. */
                p->signals.pending[sig] = 0;
                p->exit_sig = (uint8_t)SIGSEGV;
                proc_exit(139, r);
                thread_exit(139);
            }
            return 1;
        }
    }
    return 0;
}

