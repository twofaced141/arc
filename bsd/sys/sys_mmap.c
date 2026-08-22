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


/* sys_mmap.c — mmap/munmap/mprotect + per-process VMA tracking.
 *
 * Mappings are lazy on architectures with a user-fault hook (amd64,
 * arm64): mmap() only records the region; pages are materialized on
 * first access by mmap_fault_handler.  On i386 (no fault hook) pages
 * are mapped eagerly.
 *
 * File-backed mappings read from the vnode on demand (a reference to
 * the vnode is held for the region's lifetime); anonymous mappings
 * are zero-filled.  MAP_SHARED is accepted but shares no page-cache
 * state yet — the region is backed like MAP_PRIVATE. */

#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/mman.h"
#include "bsd/vfs.h"
#include "bsd/signal.h"
#include "bsd/arch.h"
#include "vmm.h"
#include "pmm.h"
#include "memory.h"
#include "debug.h"
#include "string.h"

#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))
#define ARG3(r) ((uint64_t)bsd_syscall_arg2(r))
#define ARG4(r) ((uint64_t)bsd_syscall_arg3(r))
#define ARG5(r) ((uint64_t)bsd_syscall_arg4(r))
#define ARG6(r) ((uint64_t)bsd_syscall_arg5(r))


mmap_region_t *mmap_region_find(proc_t *p, uintptr_t addr) {
    if (!p)
        return NULL;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t *r = &p->mmap_regions[i];
        if (r->used && addr >= r->start && addr < r->start + r->len)
            return r;
    }
    return NULL;
}

/* Find a free region slot, or NULL when the table is full. */
static mmap_region_t *region_slot_alloc(proc_t *p) {
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (!p->mmap_regions[i].used)
            return &p->mmap_regions[i];
    }
    return NULL;
}

static void region_slot_free(proc_t *p, mmap_region_t *r) {
    if (r->vnode) {
        vnode_put((vnode_t *)r->vnode);
        r->vnode = NULL;
    }
    r->used = 0;
}

/* Insert a region record.  Takes ownership of the vnode reference
 * (may be NULL).  Returns 0 or -ENOMEM. */
static int region_add(proc_t *p, uintptr_t start, size_t len, int prot,
                      int flags, vnode_t *vp, int64_t offset) {
    mmap_region_t *r = region_slot_alloc(p);
    if (!r)
        return -ENOMEM;
    r->start  = start;
    r->len    = len;
    r->prot   = prot;
    r->flags  = flags;
    r->vnode  = vp;
    r->offset = offset;
    r->used   = 1;
    return 0;
}

/* Drop every region and release file references (exec, exit). */
void mmap_teardown(proc_t *p) {
    if (!p)
        return;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (p->mmap_regions[i].used)
            region_slot_free(p, &p->mmap_regions[i]);
    }
    p->mmap_cursor = USER_MMAP_START;
}

/* fork: child inherits the region table; file-backed regions take
 * their own vnode reference (lazy faults in the child need it). */
void mmap_fork(proc_t *parent, proc_t *child) {
    if (!parent || !child)
        return;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t *r = &parent->mmap_regions[i];
        if (!r->used)
            continue;
        if (region_add(child, r->start, r->len, r->prot, r->flags,
                       (vnode_t *)r->vnode, r->offset) < 0)
            continue;
        if (r->vnode)
            vnode_ref((vnode_t *)r->vnode);
    }
    child->mmap_cursor = parent->mmap_cursor;
}


static int vmm_present(proc_t *p, uintptr_t page) {
    return vmm_is_page_present(p->page_dir, page);
}

/* Materialize one page of a region: allocate, zero-fill, page in file
 * contents for file-backed regions.  Returns 0 or -ENOMEM. */
static int region_materialize(proc_t *p, mmap_region_t *r, uintptr_t page) {
    void *phys = pmm_alloc_page();
    if (!phys)
        return -ENOMEM;
    uint8_t *tmp = (uint8_t *)vmm_temp_map((uintptr_t)phys);
    if (!tmp) {
        pmm_free_page(phys);
        return -ENOMEM;
    }
    memset(tmp, 0, PAGE_SIZE);

    if (r->vnode) {
        vnode_t *vp = (vnode_t *)r->vnode;
        if (vp->ops && vp->ops->read) {
            int64_t off = r->offset + (int64_t)(page - r->start);
            ssize_t n = vp->ops->read(vp, tmp, PAGE_SIZE, off);
            if (n < 0)
                n = 0;   /* zero-fill the rest */
        }
    }
    vmm_temp_unmap();

    /* VMM_NX lives at PTE bit 63 on amd64 — keep the flags wide or
     * the bit silently truncates away and pages stay executable. */
    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (r->prot & PROT_WRITE)
        flags |= VMM_WRITABLE;
#if defined(__x86_64__)
    if (!(r->prot & PROT_EXEC))
        flags |= VMM_NX;
#endif

    if (vmm_map_page(p->page_dir, (uintptr_t)phys, page, flags) < 0) {
        pmm_free_page(phys);
        return -ENOMEM;
    }
    return 0;
}


static void mmap_fault_handler(registers_t *r, uint64_t fault_addr,
                               uint32_t error_code) {
    (void)error_code;
    proc_t *p = proc_current();
    if (!p || !p->page_dir)
        return;

    mmap_region_t *reg = mmap_region_find(p, fault_addr);
    if (reg) {
        uintptr_t page = fault_addr & ~(uintptr_t)(PAGE_SIZE - 1);
        if (!vmm_present(p, page)) {
            if (region_materialize(p, reg, page) == 0)
                return;
        }
    }

    /* Unmapped address or protection violation (e.g. write to a
     * PROT_READ page): deliver a real SIGSEGV.  The handler's frame
     * restores the faulting instruction, so a handler that fixes the
     * mapping can retry; the default action terminates.  The fault is
     * synchronous, never a restartable syscall — clear the restart
     * state so the frame is not built as a syscall-restart frame. */
    p->signals.syscall_restartable = 0;
    if (signal_deliver(p, SIGSEGV, r) < 0) {
        p->signals.pending[SIGSEGV] = 1;
        p->exit_sig = SIGSEGV;
        proc_exit(139, NULL);
        thread_exit(SIGSEGV);
    }
}


int64_t sys_mmap(proc_t *p, registers_t *r) {
    uintptr_t addr    = ARG1(r);
    size_t len        = (size_t)ARG2(r);
    int prot          = (int)ARG3(r);
    int flags         = (int)ARG4(r);
    int fd            = (int)ARG5(r);
    int64_t offset    = (int64_t)ARG6(r);

    if (len == 0)
        return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    if ((flags & (MAP_PRIVATE | MAP_SHARED)) == 0 ||
        (flags & (MAP_PRIVATE | MAP_SHARED)) == (MAP_PRIVATE | MAP_SHARED))
        return -EINVAL;
    if ((flags & MAP_ANONYMOUS))
        fd = -1;   /* POSIX: fd is ignored for MAP_ANONYMOUS */
    if (offset & (PAGE_SIZE - 1))
        return -EINVAL;
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1))
            return -EINVAL;
    }

    /* Round length up to whole pages, guarding against overflow. */
    size_t pagelen = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    if (pagelen < len)
        return -ENOMEM;

    vnode_t *vp = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        filedesc_t *f = proc_fd_get(p, fd);
        if (!f || !f->used)
            return -EBADF;
        vp = (vnode_t *)f->vnode_ptr;
        if (!vp || vp->type != VREG)
            return -ENODEV;
        if (vp->ops->stat && vp->type != VCHR && vp->type != VFIFO) {
            int amode = (prot & PROT_WRITE) ? W_OK : R_OK;
            if (vfs_perm_check(p, vp, amode) < 0)
                return -EACCES;
        }
        vnode_ref(vp);   /* held until the region is torn down */
    }

    uintptr_t start = addr;
    if (!(flags & MAP_FIXED)) {
        start = p->mmap_cursor;
    }

    if (start & (PAGE_SIZE - 1))
        start &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (start + pagelen < start || start + pagelen > USER_STACK_TOP)
        return -ENOMEM;

    /* Drop any existing regions overlapping [start, start+pagelen).
     * MAP_FIXED replaces them (POSIX); plain mappings must not overlap. */
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t *old = &p->mmap_regions[i];
        if (!old->used)
            continue;
        uintptr_t oend = old->start + old->len;
        uintptr_t end  = start + pagelen;
        if (start >= oend || end <= old->start)
            continue;

        if (!(flags & MAP_FIXED)) {
            if (vp)
                vnode_put(vp);
            return -EINVAL;
        }

        uintptr_t ustart = start > old->start ? start : old->start;
        uintptr_t uend   = end < oend ? end : oend;
        for (uintptr_t pg = ustart; pg < uend; pg += PAGE_SIZE) {
            uint64_t phys = vmm_get_physical(p->page_dir, pg);
            if (phys && !(vmm_get_page_flags(p->page_dir, pg) & VMM_COW))
                pmm_free_page((void *)(uintptr_t)phys);
            vmm_unmap_page(p->page_dir, pg);
        }

        if (ustart == old->start && uend == oend) {
            region_slot_free(p, old);              /* fully replaced */
        } else if (ustart == old->start) {
            old->start = uend;                     /* shrink front */
            old->len   = oend - uend;
            old->offset += (int64_t)(uend - ustart);
        } else if (uend == oend) {
            old->len   = ustart - old->start;      /* shrink back */
        } else {
            /* Split: middle [ustart,uend) replaced; keep the tail. */
            mmap_region_t *tail = NULL;
            for (int j = 0; j < MMAP_MAX_REGIONS; j++) {
                if (!p->mmap_regions[j].used) {
                    tail = &p->mmap_regions[j];
                    break;
                }
            }
            if (tail) {
                tail->start  = uend;
                tail->len    = oend - uend;
                tail->prot   = old->prot;
                tail->flags  = old->flags;
                tail->vnode  = old->vnode;
                tail->offset = old->offset + (int64_t)(uend - old->start);
                tail->used   = 1;
                if (old->vnode)
                    vnode_ref((vnode_t *)old->vnode);
            } else {
                /* No slots left: drop the tail rather than leak. */
                for (uintptr_t pg = uend; pg < oend; pg += PAGE_SIZE) {
                    uint64_t phys = vmm_get_physical(p->page_dir, pg);
                    if (phys && !(vmm_get_page_flags(p->page_dir, pg) & VMM_COW))
                        pmm_free_page((void *)(uintptr_t)phys);
                    vmm_unmap_page(p->page_dir, pg);
                }
            }
            old->len = ustart - old->start;
        }
    }

    int ret = region_add(p, start, pagelen, prot, flags, vp, offset);
    if (ret < 0) {
        if (vp)
            vnode_put(vp);
        return ret;
    }

    /* No lazy fault hook (i386): materialize everything up front. */
#if defined(__i386__) || defined(__i686__)
    for (uintptr_t pg = start; pg < start + pagelen; pg += PAGE_SIZE) {
        mmap_region_t *nr = mmap_region_find(p, pg);
        if (nr && region_materialize(p, nr, pg) < 0)
            return -ENOMEM;
    }
#endif

    if (start + pagelen > p->mmap_cursor)
        p->mmap_cursor = (start + pagelen + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);

    return (int64_t)start;
}

int64_t sys_munmap(proc_t *p, registers_t *r) {
    uintptr_t addr = ARG1(r);
    size_t len     = (size_t)ARG2(r);

    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len == 0)
        return -EINVAL;
    size_t pagelen = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    if (pagelen < len)
        return -EINVAL;

    uintptr_t end = addr + pagelen;
    if (end < addr)
        return -EINVAL;

    /* Every byte of the range must belong to some mapping. */
    for (uintptr_t a = addr; a < end; a += PAGE_SIZE) {
        if (!mmap_region_find(p, a))
            return -EINVAL;
    }

    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t *rgn = &p->mmap_regions[i];
        if (!rgn->used)
            continue;
        uintptr_t rstart = rgn->start;
        uintptr_t rend   = rstart + rgn->len;
        if (addr >= rend || end <= rstart)
            continue;   /* no overlap */

        /* Unmap the covered pages and free them (skip COW-shared). */
        uintptr_t ustart = addr > rstart ? addr : rstart;
        uintptr_t uend   = end < rend ? end : rend;
        for (uintptr_t pg = ustart; pg < uend; pg += PAGE_SIZE) {
            uint64_t phys = vmm_get_physical(p->page_dir, pg);
            if (phys && !(vmm_get_page_flags(p->page_dir, pg) & VMM_COW))
                pmm_free_page((void *)(uintptr_t)phys);
            vmm_unmap_page(p->page_dir, pg);
        }

        if (ustart == rstart && uend == rend) {
            region_slot_free(p, rgn);         /* whole region gone */
        } else if (ustart == rstart) {
            rgn->start = uend;                /* shrink from the front */
            rgn->len   = rend - uend;
        } else if (uend == rend) {
            rgn->len   = ustart - rstart;     /* shrink from the back */
        } else {
            /* Split: the middle is removed, keep the tail as a new
             * region sharing the backing vnode. */
            if (region_add(p, uend, rend - uend, rgn->prot, rgn->flags,
                           (vnode_t *)rgn->vnode,
                           rgn->offset + (int64_t)(uend - rstart)) < 0) {
                /* Out of slots: drop the tail rather than leak. */
                for (uintptr_t pg = uend; pg < rend; pg += PAGE_SIZE) {
                    uint64_t phys = vmm_get_physical(p->page_dir, pg);
                    if (phys && !(vmm_get_page_flags(p->page_dir, pg) & VMM_COW))
                        pmm_free_page((void *)(uintptr_t)phys);
                    vmm_unmap_page(p->page_dir, pg);
                }
                region_slot_free(p, rgn);
                continue;
            }
            if (rgn->vnode)
                vnode_ref((vnode_t *)rgn->vnode);
            rgn->len = ustart - rstart;
        }
    }
    return 0;
}

int64_t sys_mprotect(proc_t *p, registers_t *r) {
    uintptr_t addr = ARG1(r);
    size_t len     = (size_t)ARG2(r);
    int prot       = (int)ARG3(r);

    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len == 0)
        return -EINVAL;
    size_t pagelen = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    if (pagelen < len)
        return -EINVAL;

    uintptr_t end = addr + pagelen;
    if (end < addr)
        return -EINVAL;

    /* The whole range must be covered by mappings (POSIX: ENOMEM). */
    for (uintptr_t a = addr; a < end; a += PAGE_SIZE) {
        if (!mmap_region_find(p, a))
            return -ENOMEM;
    }

    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t *rgn = &p->mmap_regions[i];
        if (!rgn->used)
            continue;
        uintptr_t rstart = rgn->start;
        uintptr_t rend   = rstart + rgn->len;
        if (addr >= rend || end <= rstart)
            continue;
        rgn->prot = prot;

        for (uintptr_t pg = addr > rstart ? addr : rstart;
             pg < (end < rend ? end : rend); pg += PAGE_SIZE) {
            if (!vmm_present(p, pg))
                continue;   /* not yet materialized: prot applies on fault */
            uint32_t fl = vmm_get_page_flags(p->page_dir, pg);

            /* Making a page writable must first privatize it if it is
             * copy-on-write shared with a parent/child after fork().
             * Just OR-ing VMM_WRITABLE into a COW PTE left both tasks
             * silently writing one physical page.  On allocation
             * failure report ENOMEM (POSIX allows it here). */
            if (prot & PROT_WRITE) {
#if defined(__aarch64__)
                /* arm64 PTEs carry no VMM_COW flag bit (COW pages are
                 * simply read-only), so any not-yet-writable page may
                 * be shared — break it before granting write access. */
                int cow = !(fl & VMM_WRITABLE);
#else
                int cow = (fl & VMM_COW) != 0;
#endif
                if (cow) {
                    if (!vmm_cow_break(p->page_dir, pg))
                        return -ENOMEM;
                    fl = vmm_get_page_flags(p->page_dir, pg);
                }
            }

            uint64_t new = fl & ~((uint64_t)VMM_WRITABLE);
#if defined(__x86_64__)
            new &= ~VMM_NX;
            if (!(prot & PROT_EXEC))
                new |= VMM_NX;
#endif
            if (prot & PROT_WRITE)
                new |= VMM_WRITABLE;
            vmm_map_page(p->page_dir, vmm_get_physical(p->page_dir, pg), pg, new);
        }
    }
    return 0;
}


void mmap_init(void) {
#if defined(__x86_64__) || defined(__aarch64__)
    vmm_register_fault_handler(mmap_fault_handler);
#endif
    log_printf(LOG_LEVEL_DEBUG, "mmap: init (regions=%d)\r\n", MMAP_MAX_REGIONS);
}
