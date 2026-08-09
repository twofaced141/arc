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

