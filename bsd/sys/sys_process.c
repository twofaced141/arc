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


#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/signal.h"
#include "bsd/time.h"
#include "bsd/arch.h"
#include "bsd/vfs.h"
#include "bsd/uipc/futex.h"
#include "debug.h"
#include "clockevent.h"
#include "string.h"
#include "vmm.h"
#include "thread.h"
#include "scheduler.h"
#include "pmm.h"
#include "memory.h"

/* Arg extractors */
#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))
#define ARG3(r) ((uint64_t)bsd_syscall_arg2(r))
#define ARG4(r) ((uint64_t)bsd_syscall_arg3(r))
#define ARG5(r) ((uint64_t)bsd_syscall_arg4(r))

int64_t sys_exit(proc_t *p, registers_t *r) {
    int exitcode = (int)ARG1(r);
    debug_printf("sys_exit: pid=%d, code=%d\r\n", p->pid, exitcode);
    if (p->is_thread) {
        /* A clone thread: tear down only the thread, leave the shared
         * address space alive for the rest of the group. */
        proc_thread_exit(exitcode);
        thread_exit(exitcode);
        return 0; /* never reached */
    }
    proc_exit(exitcode, r);
    /* Stop the thread — we must not return into the syscall handler
       after the process has been torn down.  thread_exit() never
       returns; it halts with cli;hlt after cleanup. */
    thread_exit(exitcode);
    return 0; /* never reached */
}

int64_t sys_fork(proc_t *p, registers_t *r) {
    (void)p;
    int ret = proc_fork(r);
    return ret;
}

int64_t sys_getpid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->pid;
}

int64_t sys_getppid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->ppid;
}

int64_t sys_waitpid(proc_t *p, registers_t *r) {
    (void)p;
    pid_t child_pid = (pid_t)ARG1(r);
    int *ustatus     = (int *)ARG2(r);
    int options      = (int)ARG3(r);
    int status = 0;

    pid_t ret = proc_waitpid(child_pid, &status, options);
    if (ret > 0 && ustatus)
        copy_to_user(ustatus, &status, sizeof(int));
    return (int64_t)ret;
}

int64_t sys_getuid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->uid;
}

int64_t sys_geteuid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->euid;
}

int64_t sys_getgid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->gid;
}

int64_t sys_getegid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->egid;
}

int64_t sys_setuid(proc_t *p, registers_t *r) {
    int uid = (int)ARG1(r);
    if (uid < 0)
        return -EINVAL;
    if (p->euid != 0 && p->uid != (uint32_t)uid && p->euid != (uint32_t)uid)
        return -EPERM;
    if (p->euid == 0) {
        p->uid = p->euid = (uint32_t)uid;
    } else {
        p->euid = (uint32_t)uid;
    }
    return 0;
}

int64_t sys_setgid(proc_t *p, registers_t *r) {
    int gid = (int)ARG1(r);
    if (gid < 0)
        return -EINVAL;
    if (p->euid != 0 && p->gid != (uint32_t)gid && p->egid != (uint32_t)gid)
        return -EPERM;
    if (p->euid == 0) {
        p->gid = p->egid = (uint32_t)gid;
    } else {
        p->egid = (uint32_t)gid;
    }
    return 0;
}

int64_t sys_execve(proc_t *p, registers_t *r) {
    int rc = proc_execve(r);
    if (rc < 0 && p && p->pid == 1)
        panic_simple("init: execve failed");
    return rc;
}

/* Grow or shrink the heap to new_brk, page-granular.  On failure the
 * break is left unchanged (already-mapped pages are rolled back), per
 * POSIX brk() semantics.  Returns 0 on success, -ENOMEM otherwise. */
static int proc_brk_resize(proc_t *p, uint64_t new_brk) {
    uint64_t old_top = (p->heap_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t new_top = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t vaddr = old_top;

    if (new_top > old_top) {
        for (; vaddr < new_top; vaddr += PAGE_SIZE) {
            void *phys = pmm_alloc_page();
            if (!phys)
                goto rollback;
            uint8_t *tmp = (uint8_t *)vmm_temp_map((uintptr_t)phys);
            if (!tmp) {
                pmm_free_page(phys);
                goto rollback;
            }
            memset(tmp, 0, PAGE_SIZE);
            vmm_temp_unmap();
            if (vmm_map_page(p->page_dir, (uintptr_t)phys, vaddr,
                             VMM_PRESENT | VMM_USER | VMM_WRITABLE) < 0) {
                pmm_free_page(phys);
                goto rollback;
            }
        }
    } else if (new_top < old_top) {
        for (uint64_t rv = new_top; rv < old_top; rv += PAGE_SIZE) {
            uint64_t phys = vmm_get_physical(p->page_dir, rv);
            vmm_unmap_page(p->page_dir, rv);
            if (phys)
                pmm_free_page((void *)(uintptr_t)phys);
        }
    }
    return 0;

rollback:
    for (uint64_t rv = old_top; rv < vaddr; rv += PAGE_SIZE) {
        uint64_t phys = vmm_get_physical(p->page_dir, rv);
        vmm_unmap_page(p->page_dir, rv);
        if (phys)
            pmm_free_page((void *)(uintptr_t)phys);
    }
    return -ENOMEM;
}

int64_t sys_brk(proc_t *p, registers_t *r) {
    uint64_t new_brk = ARG1(r);
    uint64_t old_brk = p->heap_end;

    /* brk(0) is the standard query idiom. */
    if (new_brk == 0 || new_brk == old_brk)
        return (int64_t)old_brk;
    if (new_brk < USER_HEAP_START || new_brk > USER_MMAP_START)
        return (int64_t)old_brk;
    if (proc_brk_resize(p, new_brk) < 0)
        return (int64_t)old_brk;
    p->heap_end = new_brk;
    return (int64_t)new_brk;
}

int64_t sys_sbrk(proc_t *p, registers_t *r) {
    intptr_t increment = (intptr_t)ARG1(r);
    uint64_t old_brk = p->heap_end;
    uint64_t new_brk;

    if (increment >= 0) {
        new_brk = old_brk + (uint64_t)increment;
        if (new_brk < old_brk)   /* overflow */
            return -1;
    } else {
        intptr_t dec = -increment;
        if ((uint64_t)dec > old_brk - USER_HEAP_START)
            return -1;           /* below heap start */
        new_brk = old_brk - (uint64_t)dec;
    }

    if (new_brk < USER_HEAP_START || new_brk > USER_MMAP_START)
        return -1;
    if (proc_brk_resize(p, new_brk) < 0)
        return -1;
    p->heap_end = new_brk;
    return (int64_t)old_brk;
}

int64_t sys_nanosleep(proc_t *p, registers_t *r) {
    const struct timespec *ureq = (const struct timespec *)ARG1(r);
    struct timespec *urem       = (struct timespec *)ARG2(r);
    struct timespec req;

    if (copy_from_user(&req, ureq, sizeof(req)) != 0)
        return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L)
        return -EINVAL;

    /* 100 Hz tick counter: 1 tick = 10 ms. */
    uint64_t ticks = (uint64_t)req.tv_sec * 100 +
                     (uint64_t)req.tv_nsec / 10000000L;
    uint64_t deadline = clockevent_get_ticks() + ticks;

    int rc = waitq_sleep_timeout(&p->waitq, deadline);

    if (rc < 0) {
        /* Interrupted by a signal: report the time left in rem. */
        uint64_t now = clockevent_get_ticks();
        uint64_t left = (now < deadline) ? (deadline - now) : 0;
        struct timespec rem;
        rem.tv_sec  = (int64_t)(left / 100);
        rem.tv_nsec = (long)((left % 100) * 10000000L);
        if (urem)
            copy_to_user(urem, &rem, sizeof(rem));
    }
    return rc;
}

int64_t sys_dup(proc_t *p, registers_t *r) {
    int oldfd = (int)ARG1(r);
    int newfd = proc_fd_dup(p, p, oldfd);
    return newfd;
}

int64_t sys_getcwd(proc_t *p, registers_t *r) {
    char *buf = (char *)ARG1(r);
    size_t size = (size_t)ARG2(r);

    size_t len = strlen(p->cwd) + 1;
    if (len > size)
        return -ERANGE;
    if (copy_to_user(buf, p->cwd, (uint32_t)len) != 0)
        return -EFAULT;
    return (int64_t)len;
}

int64_t sys_chdir(proc_t *p, registers_t *r) {
    char kpath[256];
    int err = vfs_copy_path(p, (const char *)ARG1(r), kpath);
    if (err < 0)
        return err;

    vnode_t *vp = vfs_lookup(kpath);
    if (!vp)
        return -ENOENT;
    if (vp->type != VDIR) {
        vnode_put(vp);
        return -ENOTDIR;
    }
    if (vfs_perm_check(p, vp, X_OK) < 0) {
        vnode_put(vp);
        return -EACCES;
    }

    strncpy(p->cwd, kpath, CWD_MAX - 1);
    p->cwd[CWD_MAX - 1] = '\0';
    vnode_put(vp);
    return 0;
}

static uint64_t sys_get_ticks(void) {
    return clockevent_get_ticks();
}

int64_t sys_uptime(proc_t *p, registers_t *r) {
    (void)p;
    (void)r;
    return (int)(sys_get_ticks() / 100);
}

int64_t sys_sleep(proc_t *p, registers_t *r) {
    (void)p;
    uint32_t seconds = ARG1(r);
    uint64_t deadline = sys_get_ticks() + (uint64_t)(seconds * 100);
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("sti");
    while (sys_get_ticks() < deadline)
        __asm__ __volatile__("hlt");
#else
    __asm__ __volatile__("msr daifclr, #2");
    while (sys_get_ticks() < deadline)
        __asm__ __volatile__("wfi");
#endif

    return 0;
}

int64_t sys_get_free_pages(proc_t *p, registers_t *r) {
    (void)p;
    (void)r;
    return (int)pmm_get_free_pages();
}

int64_t sys_get_total_pages(proc_t *p, registers_t *r) {
    (void)p;
    (void)r;
    return (int)pmm_get_total_pages();
}

int64_t sys_gettid(proc_t *p, registers_t *r) {
    (void)r;
    return (int)p->pid;
}

/* clone(flags, stack, parent_tid, tls, child_tid)
 *   rdi=flags, rsi=stack, rdx=parent_tid, r10=tls, r8=child_tid */
int64_t sys_clone(proc_t *p, registers_t *r) {
    (void)p;
    unsigned long flags = (unsigned long)ARG1(r);
    uintptr_t stack     = ARG2(r);
    uintptr_t parent_tid = ARG3(r);
    uintptr_t tls        = ARG4(r);
    uintptr_t child_tid  = ARG5(r);

    int rc = proc_clone(r, flags, stack, parent_tid, tls, child_tid);
    if (rc < 0)
        return rc;

    /* The kernel writes the new tid to child_tid before the syscall
     * returns so the child can publish its own id without racing. */
    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        int32_t ctid = rc;
        copy_to_user((void *)child_tid, &ctid, sizeof(ctid));
    }
    return rc;
}

/* futex(uaddr, op, val, uaddr2|timeout, val3)
 *   WAIT/WAIT_BITSET  : arg3 = timeout (timespec) | NULL; returns EAGAIN
 *                       when *uaddr != val, 0 on wake, -ETIMEDOUT/-EINTR.
 *   WAKE/WAKE_BITSET  : wakes up to val; returns count.
 *   REQUEUE/...       : returns nwoken.
 * val3 (arg5) carries the bitset / compare value respectively.
 */
int64_t sys_futex(proc_t *p, registers_t *r) {
    (void)p;
    uintptr_t uaddr = ARG1(r);
    int opraw       = (int)ARG2(r);
    int val         = (int)ARG3(r);
    uintptr_t arg4  = ARG4(r);   /* timeout ptr (WAIT) | uaddr2 (REQUEUE) */
    uintptr_t arg5  = ARG5(r);   /* ilen (CMP_REQUEUE) / nr_move (REQUEUE) */

    int op = opraw & (int)FUTEX_CMD_MASK;

    switch (op) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        struct timespec ts;
        uint64_t deadline = 0;   /* 0 == wait forever */
        if (arg4) {
            if (copy_from_user(&ts, (const void *)arg4, sizeof(ts)) != 0)
                return -EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
                return -EINVAL;
            uint64_t ticks = (uint64_t)ts.tv_sec * 100 +
                             (uint64_t)ts.tv_nsec / 10000000L;
            deadline = clockevent_get_ticks() + ticks + 1;
        }
        return futex_wait(uaddr, (uint32_t)val, deadline);
    }

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return futex_wake(uaddr, val);

    case FUTEX_REQUEUE: {
        uintptr_t uaddr2 = arg4;
        int nmove = (int)arg5;
        return futex_requeue(uaddr, uaddr2, val, nmove);
    }

    default:
        return -ENOSYS;
    }
}

