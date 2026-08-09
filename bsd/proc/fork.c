#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/arch.h"
#include "thread.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"
#include "memory.h"
#include "isr.h"

int proc_fork(registers_t *r) {
    proc_t *parent = proc_current();
    if (!parent) {
        log_print(LOG_LEVEL_ERROR, "proc_fork: no current process\r\n");
        return -1;
    }

    proc_t *child = proc_alloc(parent->pid);
    if (!child) {
        log_print(LOG_LEVEL_ERROR, "proc_fork: proc_alloc failed\r\n");
        return -ENOMEM;
    }

    page_directory_t *child_dir = vmm_create_directory();
    if (!child_dir) {
        log_print(LOG_LEVEL_ERROR, "proc_fork: vmm_create_directory failed\r\n");
        proc_free(child);
        return -ENOMEM;
    }
    child->page_dir = child_dir;

    vmm_fork_cow_pages(parent->page_dir, child_dir);

    thread_t *child_thread = thread_create(bsd_entry(r), child_dir, 1);
    if (!child_thread) {
        log_print(LOG_LEVEL_ERROR, "proc_fork: thread_create failed\r\n");
        vmm_free_directory(child_dir);
        proc_free(child);
        return -ENOMEM;
    }
    child->thread = child_thread;

    arch_fork_setup_regs(child_thread, r);

    for (int i = 0; i < parent->fd_capacity; i++) {
        if (parent->fds[i].used) {
            proc_fd_dup(child, parent, i);
        }
    }

    memcpy(&child->signals, &parent->signals, sizeof(sigstate_t));

    /* A fork from inside a signal handler: the child has no sigframe of
     * its own (its registers were snapshotted at fork) and no syscall is
     * in flight — reset the delivery/restart state so signals work. */
    child->signals.in_signal = 0;
    child->signals.sigframe_addr = 0;
    child->signals.syscall_restartable = 0;
    child->signals.restart_frame = 0;

    /* Inherit the parent's mmap regions (file-backed regions take their
     * own vnode reference for lazy faults in the child). */
    mmap_fork(parent, child);

    child->ppid = parent->pid;
    child->tgid = child->pid;    /* group leader of its own thread group */
    child->pgrp = parent->pgrp;
    child->session = parent->session;
    child->uid  = parent->uid;
    child->euid = parent->euid;
    child->gid  = parent->gid;
    child->egid = parent->egid;
    child->umask = parent->umask;
    memcpy(child->rlim, parent->rlim, sizeof(child->rlim));
    strncpy(child->cwd, parent->cwd, CWD_MAX - 1);
    child->cwd[CWD_MAX - 1] = '\0';

    /* Link into the parent's children list (for waitpid/reparenting). */
    child->sibling = parent->children;
    parent->children = child;

    scheduler_add_thread(child_thread);

    log_printf(LOG_LEVEL_DEBUG, "proc_fork: parent pid=%d, child pid=%d\r\n", parent->pid, child->pid);

    return child->pid;
}

