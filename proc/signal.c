#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "vmm.h"
#include "panic.h"

void signal_init_process(process_t *proc) {
    for (int i = 0; i < 32; i++)
        proc->sigactions[i].sa_handler = SIG_DFL;
    proc->signal_pending = 0;
    proc->signal_blocked = 0;
    /* SIGKILL and SIGSTOP cannot be caught or ignored; SIGKILL stays DFL */
}

static int signal_default_action(int signum) {
    switch (signum) {
    case SIGKILL:
    case SIGTERM:
    case SIGINT:
    case SIGQUIT:
    case SIGABRT:
    case SIGSEGV:
    case SIGBUS:
    case SIGFPE:
    case SIGILL:
        return 1; /* terminate */
    default:
        return 0; /* ignore */
    }
}

static int find_pending_signal(process_t *proc) {
    uint32_t pending = proc->signal_pending & ~proc->signal_blocked;
    if (pending == 0) return 0;
    for (int i = 1; i < 32; i++) {
        if (pending & (1 << i))
            return i;
    }
    return 0;
}

int sys_sigaction(int signum, const sigaction_t *act, sigaction_t *oldact) {
    if (signum < 1 || signum >= 32 || signum == SIGKILL)
        return -1;

    process_t *cur = scheduler_current_process();
    if (!cur) return -1;

    if (oldact) {
        sigaction_t kold = cur->sigactions[signum];
        if (copy_to_user(oldact, &kold, sizeof(sigaction_t)) < 0)
            return -1;
    }

    if (act) {
        sigaction_t kact;
        if (copy_from_user(&kact, act, sizeof(sigaction_t)) < 0)
            return -1;
        cur->sigactions[signum] = kact;
    }

    return 0;
}

void deliver_signal(registers_t *r) {
    process_t *proc = scheduler_current_process();
    if (!proc) return;

    int signum = find_pending_signal(proc);
    if (!signum) return;

    proc->signal_pending &= ~(1 << signum);

    void (*handler)(int) = proc->sigactions[signum].sa_handler;

    if (handler == SIG_DFL) {
        if (signal_default_action(signum)) {
            proc->exit_code = 128 + signum;
            proc->kernel_esp = (uint32_t)r;
            process_exit(proc);
            return;
        }
        return;
    }

    if (handler == SIG_IGN)
        return;

    /* Custom handler – build signal frame on user stack */
    uint32_t old_esp = r->useresp;
    uint32_t old_eip = r->eip;

    uint32_t trampoline[2];
    trampoline[0] = 0x00001AB8;  /* mov eax, 26 (SYSCALL_SIGRETURN) */
    trampoline[1] = 0x009080CD;  /* int 0x80; nop */

    uint32_t esp = old_esp;

    /* Push saved_esp */
    esp -= 4;
    if (esp < USER_HEAP_START) return;
    *(uint32_t *)esp = old_esp;

    /* Push saved_eip */
    esp -= 4;
    if (esp < USER_HEAP_START) return;
    *(uint32_t *)esp = old_eip;

    /* Push trampoline (8 bytes) */
    esp -= 8;
    if (esp < USER_HEAP_START) return;
    ((uint32_t *)esp)[0] = trampoline[0];
    ((uint32_t *)esp)[1] = trampoline[1];
    uint32_t tramp_addr = esp;

    /* Push signum */
    esp -= 4;
    if (esp < USER_HEAP_START) return;
    uint32_t sig_val = (uint32_t)signum;
    *(uint32_t *)esp = sig_val;

    /* Push return address (trampoline) */
    esp -= 4;
    if (esp < USER_HEAP_START) return;
    *(uint32_t *)esp = tramp_addr;

    r->useresp = esp;
    r->eip = (uint32_t)handler;
}

int sys_sigreturn(registers_t *r) {
    uint32_t *frame = (uint32_t *)r->useresp;
    r->eip = frame[3];    /* saved_eip */
    r->useresp = frame[4]; /* saved_esp */
    /* eax already holds the return value (0 from sigreturn) */
    return 0;
}

int sys_kill_sig(int pid, int signum) {
    if (signum < 1 || signum >= 32)
        return -1;

    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
        if ((int)p->pid != pid) continue;

        if (signum == SIGKILL) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return process_kill(pid);
        }

        p->signal_pending |= (1 << signum);
        if (p->state == PROC_BLOCKED) {
            p->state = PROC_READY;
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        return 0;
    }

    spin_unlock_irqrestore(&proc_lock, flags);
    return -1;
}
