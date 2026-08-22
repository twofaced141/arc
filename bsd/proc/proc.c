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
#include "bsd/signal.h"
#include "bsd/vfs.h"
#include "bsd/uipc/futex.h"
#include "bsd/cap.h"
#include "thread.h"
#include "scheduler.h"
#include "clockevent.h"

/* T7 debug toggle (defined in amd64 scheduler.c; amd64-only) */
#if defined(__x86_64__)
extern volatile int sched_dbg;
#endif
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"
#include "memory.h"

static proc_t proc_table[PROC_MAX];
static spinlock_t proc_lock = SPINLOCK_INIT;
static pid_t next_pid = 1;
static uint32_t pid_bitmap[(PROC_MAX + 31) / 32];
static proc_t *live_list;   /* live processes, for O(live) pid/thread lookups */


void waitq_init(waitq_t *wq) {
    wq->head = NULL;
    wq->lock = SPINLOCK_INIT;
}

/* Block the current process on wq until woken.  Returns -EINTR when a
 * signal is pending (the caller then aborts its syscall), 0 otherwise. */
int waitq_sleep(waitq_t *wq) {
    proc_t *p = proc_current();
    if (!p)
        return -EINTR;

    if (signal_has_pending(p))
        return -EINTR;

    uint32_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    p->wait_next = wq->head;
    wq->head = p;
    spin_unlock_irqrestore(&wq->lock, flags);

    p->state = PRS_SLEEP;
    scheduler_block_current();
    thread_yield();

    /* Woken — remove ourselves from the queue (a signal wakeup may
     * have left us linked). */
    spin_lock_irqsave(&wq->lock, &flags);
    if (wq->head) {
        proc_t **pp = &wq->head;
        while (*pp) {
            if (*pp == p) {
                *pp = p->wait_next;
                break;
            }
            pp = &(*pp)->wait_next;
        }
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    p->state = PRS_RUNNING;
    return signal_has_pending(p) ? -EINTR : 0;
}

/* Block the current process on wq until woken or the clockevent tick
 * counter reaches deadline.  The thread is registered on both the wait
 * queue (signal wakeups via proc_wakeup/waitq_wake_all) and the
 * scheduler sleep queue (timeout via sleep_wake_tick); whichever fires
 * first unblocks it.  Returns -EINTR when a signal is pending, 0 when
 * the deadline elapsed or the process was woken. */
int waitq_sleep_timeout(waitq_t *wq, uint64_t deadline) {
    proc_t *p = proc_current();
    if (!p)
        return -EINTR;

    if (signal_has_pending(p))
        return -EINTR;

    if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
        return 0;

    uint32_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    p->wait_next = wq->head;
    wq->head = p;
    spin_unlock_irqrestore(&wq->lock, flags);

    p->state = PRS_SLEEP;

    int sret = scheduler_sleep_ticks(deadline);
    if (sret == 1) {
        /* Thread was blocked on the scheduler sleep queue: yield the
         * CPU; the timer tick (sleep_wake_tick) or a signal wakeup
         * resumes us. */
        thread_yield();
    } else if (sret < 0) {
        /* Sleep queue full or current is the idle thread: poll the
         * deadline, but stay linked on the waitq so a signal still
         * interrupts us. */
        while ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) < 0) {
            thread_yield();
            if (signal_has_pending(p))
                break;
        }
    }
    /* sret == 0: deadline already passed, no blocking needed. */

    /* Woken — remove ourselves from the queue (a signal wakeup may
     * have left us linked). */
    spin_lock_irqsave(&wq->lock, &flags);
    if (wq->head) {
        proc_t **pp = &wq->head;
        while (*pp) {
            if (*pp == p) {
                *pp = p->wait_next;
                break;
            }
            pp = &(*pp)->wait_next;
        }
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    p->state = PRS_RUNNING;
    return signal_has_pending(p) ? -EINTR : 0;
}

/* Variant of waitq_sleep_timeout for callers that have already linked
 * the current process into wq while holding the wakers' lock (futex.c
 * does this so a wake cannot observe an empty queue after the value
 * check).  Skips the linking step; also refrains from sleeping if a
 * waker has already popped us (it sets our state out of PRS_SLEEP and
 * scheduler_unblock_thread is a no-op for runnable threads). */
int waitq_sleep_timeout_linked(waitq_t *wq, uint64_t deadline) {
    proc_t *p = proc_current();
    if (!p)
        return -EINTR;

    if (signal_has_pending(p))
        return -EINTR;

    if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
        return 0;

    /* If a waker already popped us from the queue while we were still
     * runnable (it sets PRS_RUNNING, and scheduler_unblock_thread is a
     * no-op for non-blocked threads), the wake is already delivered:
     * do not sleep, or the wake would be lost. */
    if (p->state != PRS_SLEEP)
        return 0;

    /* Set ourselves up to sleep only if nobody woke us in the window
     * between the caller's link and now. */
    p->state = PRS_SLEEP;

    int sret = scheduler_sleep_ticks(deadline);
    if (sret == 1) {
        thread_yield();
    } else if (sret < 0) {
        while ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) < 0) {
            thread_yield();
            if (signal_has_pending(p))
                break;
        }
    }

    /* Remove ourselves from the queue if still linked (a signal or
     * timeout wakeup can leave us linked; a futex wake has already
     * popped us). */
    uint32_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    if (wq->head) {
        proc_t **pp = &wq->head;
        while (*pp) {
            if (*pp == p) {
                *pp = p->wait_next;
                break;
            }
            pp = &(*pp)->wait_next;
        }
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    p->state = PRS_RUNNING;
    return signal_has_pending(p) ? -EINTR : 0;
}

static void waitq_wake_locked(waitq_t *wq, proc_t *only) {
    proc_t **pp = &wq->head;
    while (*pp) {
        proc_t *w = *pp;
        if (only && w != only) {
            pp = &w->wait_next;
            continue;
        }
        *pp = w->wait_next;
        w->wait_next = NULL;
        if (w->state == PRS_SLEEP)
            w->state = PRS_RUNNING;
        scheduler_unblock_thread(w->thread);
        if (only)
            return;
    }
}

void waitq_wake_all(waitq_t *wq) {
    uint32_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    waitq_wake_locked(wq, NULL);
    spin_unlock_irqrestore(&wq->lock, flags);
}

void waitq_wake_one(waitq_t *wq) {
    uint32_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    waitq_wake_locked(wq, wq->head);
    spin_unlock_irqrestore(&wq->lock, flags);
}

/* Wake a process that may be blocked in a syscall — used by signal
 * delivery so blocked reads/waits are interrupted (EINTR). */
void proc_wakeup(proc_t *p) {
    if (!p || !p->thread)
        return;
    if (p->state == PRS_SLEEP)
        p->state = PRS_RUNNING;
    scheduler_unblock_thread(p->thread);
}


static void proc_children_unlink(proc_t *parent, proc_t *child) {
    if (!parent)
        return;
    proc_t **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->sibling;
            child->sibling = NULL;
            return;
        }
        pp = &(*pp)->sibling;
    }
}

void proc_init(void) {
    memset(proc_table, 0, sizeof(proc_table));
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    next_pid = 1;
    pid_bitmap[0] = 1;
    live_list = NULL;
    log_printf(LOG_LEVEL_DEBUG, "proc: process table initialized (PROC_MAX=%d)\r\n", PROC_MAX);
}

pid_t proc_alloc_pid(void) {
    /* Caller must hold proc_lock */
    for (pid_t pid = 1; pid < PROC_MAX; pid++) {
        uint32_t idx = pid / 32;
        uint32_t bit = pid % 32;
        if ((pid_bitmap[idx] & (1u << bit)) == 0) {
            pid_bitmap[idx] |= (1u << bit);
            if (pid >= next_pid)
                next_pid = pid + 1;
            return pid;
        }
    }
    return -1;
}

/* Caller must hold proc_lock */
static void proc_release_pid_locked(pid_t pid) {
    if (pid <= 0 || pid >= PROC_MAX)
        return;
    uint32_t idx = pid / 32;
    uint32_t bit = pid % 32;
    pid_bitmap[idx] &= ~(1u << bit);
}

void proc_free_pid(pid_t pid) {
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    proc_release_pid_locked(pid);
    spin_unlock_irqrestore(&proc_lock, flags);
}

proc_t *proc_alloc(pid_t ppid) {
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);

    proc_t *p = NULL;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].pid == PROC_NULL) {
            p = &proc_table[i];
            break;
        }
    }

    if (!p) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return NULL;
    }

    pid_t pid = proc_alloc_pid();
    if (pid < 0) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return NULL;
    }

    memset(p, 0, sizeof(proc_t));
    p->pid = pid;
    p->ppid = ppid;
    p->state = PRS_NORMAL;
    p->pgrp = pid;
    p->session = pid;
    p->uid = p->euid = p->gid = p->egid = 0;
    p->umask = 022;
    p->priority = 0;
    p->nice = 0;
    p->mmap_next = USER_MMAP_START;
    p->heap_end = USER_HEAP_START;
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    p->fd_lock = SPINLOCK_INIT;
    waitq_init(&p->waitq);
    rlimit_init_defaults(p->rlim);

    p->fds = (filedesc_t *)kmalloc(sizeof(filedesc_t) * FD_INITIAL);
    if (!p->fds) {
        proc_release_pid_locked(pid);
        spin_unlock_irqrestore(&proc_lock, flags);
        return NULL;
    }
    memset(p->fds, 0, sizeof(filedesc_t) * FD_INITIAL);
    p->fd_capacity = FD_INITIAL;

    p->next = live_list;
    live_list = p;

    spin_unlock_irqrestore(&proc_lock, flags);
    return p;
}

void proc_free(proc_t *p) {
    if (!p) {
        return;
    }

    /* Tear down the address space ONLY if no other live process still
     * runs on it.  CLONE_THREAD group members share the leader's
     * page_dir; freeing it while they are alive (leader reaped before
     * its threads exit) would yank the page tables out from under
     * running threads.  A leak is safe; a free here is not. */
    int dir_shared = 0;
    uint32_t lflags;
    spin_lock_irqsave(&proc_lock, &lflags);
    for (proc_t *q = live_list; q; q = q->next) {
        if (q != p && q->pid != PROC_NULL && q->page_dir == p->page_dir) {
            dir_shared = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, lflags);

    if (p->page_dir && !dir_shared) {
        vmm_free_directory(p->page_dir);
    }
    p->page_dir = NULL;

    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    pid_t pid = p->pid;
    p->pid = PROC_NULL;

    proc_release_pid_locked(pid);

    /* Unlink from the live list */
    if (live_list == p) {
        live_list = p->next;
    } else {
        for (proc_t *q = live_list; q && q->next; q = q->next) {
            if (q->next == p) {
                q->next = p->next;
                break;
            }
        }
    }

    if (p->fds) {
        kfree(p->fds);
        p->fds = NULL;
    }
    p->fd_capacity = 0;
    memset(p, 0, sizeof(proc_t));
    spin_unlock_irqrestore(&proc_lock, flags);
}

proc_t *proc_find(pid_t pid) {
    if (pid <= 0 || pid >= PROC_MAX) {
        return NULL;
    }
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    for (proc_t *q = live_list; q; q = q->next) {
        if (q->pid == pid) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return q;
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return NULL;
}

/* Collect pids of live processes matching the kill(2) target class:
 *   pid == 0   — the caller's process group (pgrp)
 *   pid == -1  — every live process
 *   pid <  -1  — process group -pid
 * Matching happens under proc_lock, but only pids are collected; the
 * caller delivers without holding the lock (delivery may call proc_exit,
 * which takes proc_lock again).  Returns the number collected (may be
 * less than the actual number of matches if max is exhausted). */
int proc_collect_kill_targets(pid_t pid, pid_t caller_pgrp,
                              pid_t *out, int max) {
    uint32_t flags;
    int n = 0;
    spin_lock_irqsave(&proc_lock, &flags);
    for (proc_t *q = live_list; q; q = q->next) {
        int match;
        if (pid == 0)
            match = (q->pgrp == caller_pgrp);
        else if (pid == -1)
            match = 1;
        else
            match = (q->pgrp == -pid);
        if (match && n < max)
            out[n++] = q->pid;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return n;
}

proc_t *proc_current(void) {
    thread_t *t = thread_current();
    if (!t) {
        return NULL;
    }
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    for (proc_t *q = live_list; q; q = q->next) {
        if (q->pid != PROC_NULL && q->thread == t) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return q;
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);    return NULL;
}

/* Number of live (non-NULL) processes — for sysinfo(2). */
int proc_count(void) {
    int n = 0;
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    for (proc_t *q = live_list; q; q = q->next)
        if (q->pid != PROC_NULL)
            n++;
    spin_unlock_irqrestore(&proc_lock, flags);
    return n;
}

/* Default resource limits installed at process creation (POSIX:
 * getrlimit returns sensible defaults; RLIMIT_STACK is what the
 * dynamic linker uses to pick the initial stack size). */
void rlimit_init_defaults(rlimit_t *rlim) {
    for (int i = 0; i < RLIM_NLIMITS; i++) {
        rlim[i].rlim_cur = RLIM_INFINITY;
        rlim[i].rlim_max = RLIM_INFINITY;
    }
    rlim[RLIMIT_NOFILE].rlim_cur = FD_MAX;
    rlim[RLIMIT_NOFILE].rlim_max = FD_MAX;
    rlim[RLIMIT_STACK].rlim_cur = 8 * 1024 * 1024;
    rlim[RLIMIT_STACK].rlim_max = 8 * 1024 * 1024;
}

int proc_fd_alloc(proc_t *p) {
    if (!p) {
        return -1;
    }
    uint32_t flags;
    spin_lock_irqsave(&p->fd_lock, &flags);
    if (!p->fds) {
        spin_unlock_irqrestore(&p->fd_lock, flags);
        return -1;
    }

    for (int i = 0; i < p->fd_capacity; i++) {
        if (p->fds[i].used == 0) {
            p->fds[i].used = 1;
            p->fds[i].fd = i;
            spin_unlock_irqrestore(&p->fd_lock, flags);
            return i;
        }
    }

    /* Table full — grow it, doubling up to FD_MAX */
    if (p->fd_capacity >= FD_MAX) {
        spin_unlock_irqrestore(&p->fd_lock, flags);
        return -1;
    }
    int newcap = p->fd_capacity * 2;
    if (newcap > FD_MAX)
        newcap = FD_MAX;
    filedesc_t *nf = (filedesc_t *)kmalloc(sizeof(filedesc_t) * newcap);
    if (!nf) {
        spin_unlock_irqrestore(&p->fd_lock, flags);
        return -1;
    }
    memset(nf, 0, sizeof(filedesc_t) * newcap);
    memcpy(nf, p->fds, sizeof(filedesc_t) * p->fd_capacity);
    kfree(p->fds);
    p->fds = nf;
    p->fd_capacity = newcap;

    int fd = p->fd_capacity / 2;   /* first slot of the grown half */
    p->fds[fd].used = 1;
    p->fds[fd].fd = fd;
    spin_unlock_irqrestore(&p->fd_lock, flags);
    return fd;
}

int proc_fd_dealloc(proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= p->fd_capacity || !p->fds) {
        return -1;
    }
    uint32_t flags;
    spin_lock_irqsave(&p->fd_lock, &flags);
    if (p->fds[fd].used) {
        p->fds[fd].used = 0;
        spin_unlock_irqrestore(&p->fd_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&p->fd_lock, flags);
    return -1;
}

filedesc_t *proc_fd_get(proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= p->fd_capacity || !p->fds) {
        return NULL;
    }
    uint32_t flags;
    spin_lock_irqsave(&p->fd_lock, &flags);
    if (p->fds[fd].used) {
        filedesc_t *f = &p->fds[fd];
        spin_unlock_irqrestore(&p->fd_lock, flags);
        return f;
    }
    spin_unlock_irqrestore(&p->fd_lock, flags);
    return NULL;
}

int proc_fd_dup(proc_t *dst, proc_t *src, int fd) {
    if (!dst || !src || fd < 0 || fd >= src->fd_capacity || !src->fds) {
        return -1;
    }
    uint32_t flags;
    spin_lock_irqsave(&src->fd_lock, &flags);
    if (!src->fds[fd].used) {
        spin_unlock_irqrestore(&src->fd_lock, flags);
        return -1;
    }
    filedesc_t src_fd = src->fds[fd];
    spin_unlock_irqrestore(&src->fd_lock, flags);

    int new_fd = proc_fd_alloc(dst);
    if (new_fd < 0) {
        return -1;
    }

    /* A dup is another reference to the same vnode: refcount it so the
     * source's close doesn't free the vnode out from under us. */
    vnode_t *dup_vp = (vnode_t *)src_fd.vnode_ptr;
    if (dup_vp)
        vnode_ref(dup_vp);

    spin_lock_irqsave(&dst->fd_lock, &flags);
    dst->fds[new_fd] = src_fd;
    dst->fds[new_fd].fd = new_fd;
    spin_unlock_irqrestore(&dst->fd_lock, flags);

    return new_fd;
}

int proc_fd_dup2(proc_t *dst, proc_t *src, int oldfd, int newfd) {
    if (!dst || !src || oldfd < 0 || newfd < 0 || newfd >= FD_MAX) {
        return -1;
    }
    if (oldfd == newfd)
        return newfd;

    /* The source must be open. */
    if (!src->fds || oldfd >= src->fd_capacity || !src->fds[oldfd].used)
        return -1;

    /* Grow the destination table to cover newfd. */
    if (!dst->fds || newfd >= dst->fd_capacity) {
        int newcap = dst->fds ? dst->fd_capacity : FD_INITIAL;
        while (newcap <= newfd && newcap < FD_MAX)
            newcap *= 2;
        if (newcap > FD_MAX)
            newcap = FD_MAX;
        if (newfd >= newcap)
            return -1;
        filedesc_t *nf = (filedesc_t *)kmalloc(sizeof(filedesc_t) * newcap);
        if (!nf)
            return -1;
        memset(nf, 0, sizeof(filedesc_t) * newcap);
        if (dst->fds && dst->fd_capacity > 0)
            memcpy(nf, dst->fds, sizeof(filedesc_t) * dst->fd_capacity);
        kfree(dst->fds);
        dst->fds = nf;
        dst->fd_capacity = newcap;
    }

    uint32_t flags;
    spin_lock_irqsave(&dst->fd_lock, &flags);

    /* Close any existing descriptor at newfd. */
    if (dst->fds[newfd].used) {
        vnode_t *vp = (vnode_t *)dst->fds[newfd].vnode_ptr;
        if (vp)
            vnode_put(vp);
        dst->fds[newfd].used = 0;
    }

    filedesc_t oldfd_copy;
    int have_old = 0;
    if (dst == src) {
        oldfd_copy = dst->fds[oldfd];
        have_old = 1;
    }
    spin_unlock_irqrestore(&dst->fd_lock, flags);

    if (!have_old) {
        spin_lock_irqsave(&src->fd_lock, &flags);
        if (!src->fds[oldfd].used) {
            spin_unlock_irqrestore(&src->fd_lock, flags);
            return -1;
        }
        oldfd_copy = src->fds[oldfd];
        spin_unlock_irqrestore(&src->fd_lock, flags);
    }

    vnode_t *dup_vp = (vnode_t *)oldfd_copy.vnode_ptr;
    if (dup_vp)
        vnode_ref(dup_vp);

    spin_lock_irqsave(&dst->fd_lock, &flags);
    dst->fds[newfd] = oldfd_copy;
    dst->fds[newfd].fd = newfd;
    dst->fds[newfd].used = 1;
    dst->fds[newfd].cloexec = 0;   /* POSIX: dup2 clears FD_CLOEXEC */
    spin_unlock_irqrestore(&dst->fd_lock, flags);

    return newfd;
}

void proc_kill_by_signal(int sig, registers_t *r) {
    proc_t *p = proc_current();
    if (!p)
        return;
    p->exit_sig = (uint8_t)sig;
    proc_exit(0, r);
}

/* Reparent every child of `p` to init (pid 1).  Zombie children stay
 * zombie; init's waitpid will reap them. */
static void proc_reparent_children(proc_t *p) {
    proc_t *init = proc_find(1);

    for (proc_t *c = p->children; c; c = c->sibling)
        c->ppid = 1;

    if (init) {
        proc_t *last = p->children;
        if (last) {
            while (last->sibling)
                last = last->sibling;
            last->sibling = init->children;
            init->children = p->children;
            p->children = NULL;
        }
    } else if (p->children) {
        p->children = NULL;
    }
}

void proc_exit(int exitcode, registers_t *r) {
    (void)r;
    proc_t *p = proc_current();
    if (!p) return;

    log_printf(LOG_LEVEL_DEBUG, "proc_exit: pid=%d exitcode=%d\r\n", p->pid, exitcode);

    /* Drop device session handles and IRQ subscriptions
     * (capabilities die with the process). */
    dev_handles_release(p->pid);
    irq_unsubscribe_all(p->pid);

    /* Close all file descriptors */
    for (int i = 0; i < p->fd_capacity; i++) {
        if (p->fds && p->fds[i].used) {
            vnode_t *vp = (vnode_t *)p->fds[i].vnode_ptr;
            if (vp) vnode_put(vp);
            p->fds[i].used = 0;
        }
    }

    /* Drop mmap regions (releases file-backed vnode references).  The
     * mapped pages themselves are freed later with the page directory. */
    mmap_teardown(p);

    /* Orphaned children are adopted by init. */
    proc_reparent_children(p);

    p->exit_status = exitcode;
    p->state = PRS_ZOMBIE;

    /* Wake up the parent if it is waiting in waitpid. */
    proc_t *parent = proc_find(p->ppid);
    if (parent)
        waitq_wake_all(&parent->waitq);

    /* page_dir is NOT freed here — thread still runs with CR3 pointing to
       it; freeing the active PML4 would corrupt subsequent page walks. */

    /* Kill the thread */
    thread_t *t = p->thread;
    if (t) {
        p->thread = NULL;
        scheduler_remove_thread(t);
    }

    log_printf(LOG_LEVEL_DEBUG, "proc_exit: pid=%d done\r\n", p->pid);
}

/* Exit path for a clone CLONE_THREAD member.  Unlike a full process
 * exit the shared address space (page_dir) is NOT torn down — the
 * group leader and its other threads still run in it.  We:
 *   1. zero clear_child_tid in user memory and wake future joiners
 *      (the futex_join convention: wait on the tid cell, value 0 ==
 *      thread gone),
 *   2. close the thread's file descriptors,
 *   3. unlink the proc slot + release the pid.
 */
void proc_thread_exit(int exitcode) {
    (void)exitcode;
    proc_t *p = proc_current();
    if (!p)
        return;

    log_printf(LOG_LEVEL_DEBUG, "proc_thread_exit: tid=%d tgid=%d\r\n", p->pid, p->tgid);

    if (p->clear_child_tid) {
        uint32_t zero = 0;
        if (copy_to_user((void *)p->clear_child_tid, &zero, sizeof(zero)) >= 0)
            futex_wake((uintptr_t)p->clear_child_tid, 1);
    }

    for (int i = 0; i < p->fd_capacity; i++) {
        if (p->fds && p->fds[i].used) {
            vnode_t *vp = (vnode_t *)p->fds[i].vnode_ptr;
            if (vp)
                vnode_put(vp);
            p->fds[i].used = 0;
        }
    }

    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    pid_t pid = p->pid;
    p->pid = PROC_NULL;
    proc_release_pid_locked(pid);

    if (live_list == p) {
        live_list = p->next;
    } else {
        for (proc_t *q = live_list; q && q->next; q = q->next) {
            if (q->next == p) {
                q->next = p->next;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    if (p->fds) {
        kfree(p->fds);
        p->fds = NULL;
    }

    log_printf(LOG_LEVEL_DEBUG, "proc_thread_exit: tid=%d gone\r\n", pid);
}

/* waitpid(2): wait for a child.  pid > 0 waits for that specific child;
 * pid <= 0 waits for any child.  Blocks (interruptibly) until a child
 * exits or stops, unless WNOHANG is given.  Returns the child pid,
 * 0 with WNOHANG when nothing to reap, or a negative errno. */
pid_t proc_waitpid(pid_t pid, int *status, int options) {
    proc_t *p = proc_current();
    if (!p)
        return -EINVAL;

    for (;;) {
        proc_t *child = NULL;
        for (proc_t *c = p->children; c; c = c->sibling) {
            if (pid > 0) {
                if (c->pid == pid) {
                    child = c;
                    break;
                }
            } else {
                child = c;
                break;
            }
        }

        if (!child)
            return -ECHILD;

        if (child->state == PRS_ZOMBIE) {
            int st = (child->exit_status & 0xFF) << 8;
            if (child->exit_sig)
                st = (int)child->exit_sig;
            if (status)
                *status = st;
            pid_t cpid = child->pid;

            /* Unlink from the parent's children list */
            proc_children_unlink(p, child);

            proc_free(child);
            return cpid;
        }

        if (child->stopped) {
            if (options & WUNTRACED) {
                if (status)
                    *status = 0x7F | ((child->exit_sig & 0xFF) << 8);
                return child->pid;
            }
        }

        if (options & WNOHANG)
            return 0;

        /* Block until a child changes state; signal delivery wakes us.
         * waitpid is restartable: return -ERESTARTSYS so an SA_RESTART
         * handler causes the wait to be re-entered, not -EINTR. */
        int r = waitq_sleep(&p->waitq);
        if (r < 0)
            return -ERESTARTSYS;
    }
}

thread_t *kthread_create(kthread_func_t func, void *arg, const char *name) {
    (void)arg;
    page_directory_t *kernel_dir = vmm_get_kernel_directory();
    thread_t *t = thread_create((uint64_t)(uintptr_t)func, kernel_dir, 0);
    if (!t) {
        return NULL;
    }
    if (name) {
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }
    scheduler_add_thread(t);
    return t;
}