/* clone.c — clone(2): create a thread (or a fork-like child).
 *
 * The BSD layer keeps one proc_t per schedulable entity (matching the
 * waitq design), so a clone thread is a proc_t that shares the parent's
 * address space (page_dir) instead of getting a COW copy.  Per-thread
 * things that must differ — tid, tgid, TLS base, user stack, clear-tid
 * — live on the proc_t / thread_t.
 *
 * Semantics kept Unix-shaped:
 *   - CLONE_VM          share the address space (no copy)
 *   - CLONE_THREAD      join the parent's thread group (tgid = leader)
 *   - CLONE_PARENT_SETTID / CLONE_CHILD_SETTID
 *                       write the child tid to a user address
 *   - CLONE_CHILD_CLEARTID
 *                       zero the user tid + FUTEX_WAKE on thread exit
 *   - CLONE_SETTLS      per-thread TLS base (amd64: MSR_FS_BASE)
 *   - CLONE_FILES/FS    duplicated table/cwd (not shared refcounts —
 *                       correct enough for the supported workloads)
 *   - child returns 0, runs on the caller-supplied stack
 *
 * Clone threads are not linked into the parent's children list, so
 * waitpid never sees them (POSIX: only group leaders are wait-able).
 */

#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/arch.h"
#include "thread.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "debug.h"

int proc_clone(registers_t *r, unsigned long flags, uintptr_t child_stack,
               uintptr_t parent_tid, uintptr_t tls, uintptr_t child_tid) {
    proc_t *parent = proc_current();
    if (!parent)
        return -EINVAL;

    proc_t *child = proc_alloc(parent->pid);
    if (!child)
        return -ENOMEM;

    /* Address space: share under CLONE_VM, otherwise a COW copy. */
    if (flags & CLONE_VM) {
        child->page_dir = parent->page_dir;   /* shared — never freed by us */
        child->is_thread = 1;
    } else {
        child->page_dir = vmm_create_directory();
        if (!child->page_dir) {
            proc_free(child);
            return -ENOMEM;
        }
        vmm_fork_cow_pages(parent->page_dir, child->page_dir);
    }

    thread_t *child_thread = thread_create(bsd_entry(r), child->page_dir, 1);
    if (!child_thread) {
        if (child->page_dir != parent->page_dir)
            vmm_free_directory(child->page_dir);
        proc_free(child);
        return -ENOMEM;
    }
    child->thread = child_thread;

    arch_clone_setup_regs(child_thread, r, child_stack);
    arch_thread_set_tls(child_thread, tls);
    child->tls_base = tls;

    /* File descriptors: dup the table (each thread its own refs). */
    for (int i = 0; i < parent->fd_capacity; i++) {
        if (parent->fds[i].used)
            proc_fd_dup(child, parent, i);
    }

    memcpy(&child->signals, &parent->signals, sizeof(sigstate_t));

    child->ppid = parent->pid;
    child->pgrp = parent->pgrp;
    child->session = parent->session;
    child->uid  = parent->uid;
    child->euid = parent->euid;
    child->gid  = parent->gid;
    child->egid = parent->egid;
    child->umask = parent->umask;
    strncpy(child->cwd, parent->cwd, CWD_MAX - 1);
    child->cwd[CWD_MAX - 1] = '\0';
    child->heap_end = parent->heap_end;

    /* Thread-group identity */
    if (flags & CLONE_THREAD) {
        child->tgid = parent->tgid ? parent->tgid : parent->pid;
        child->is_thread = 1;
    } else {
        child->tgid = child->pid;
    }

    child->clear_child_tid = child_tid;
    child->set_child_tid = child_tid;

    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        int32_t ctid = child->pid;
        if (copy_to_user((void *)parent_tid, &ctid, sizeof(ctid)) < 0) {
            if (child->page_dir != parent->page_dir)
                vmm_free_directory(child->page_dir);
            proc_free(child);
            return -EFAULT;
        }
    }
    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        int32_t ctid = child->pid;
        copy_to_user((void *)child_tid, &ctid, sizeof(ctid));
    }

    /* NOT linked into parent->children: clone threads are not wait-able
     * children (they are reaped by their own exit path instead). */
    scheduler_add_thread(child_thread);

    debug_printf("proc_clone: parent pid=%d tgid=%d -> child pid=%d "
                 "tgid=%d vm=%d tls=0x%lx\r\n",
                 parent->pid, parent->tgid, child->pid, child->tgid,
                 child->page_dir == parent->page_dir,
                 (unsigned long)tls);

    return child->pid;
}