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


#include "bsd/vfs.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"
#include "vmm.h"

/* Vnode cache: vnodes are keyed by (mount, ino) and reused across
 * opens, so filesystem-private data (in-memory inode copies) and
 * vp->size are shared between all fds of the same file. */
/* Lifecycle:                                                         */
/*   vnode_cache_get  — hit: refcount++ on the cached vnode;          */
/*                      miss: fresh vnode (refcount 1, NOT in hash)   */
/*   fs fills vp->data/type/ops and calls vnode_cache_commit          */
/*   vnode_unref → 0: cached vnode returns to the LRU list;           */
/*                    uncommitted/doomed vnodes are destroyed         */
/*   eviction: LRU tail (refcount 0) → ops->close + kfree             */

#define VNODE_CACHE_BUCKETS 64
#define VNODE_CACHE_MAX     256

static vnode_t *vn_hash[VNODE_CACHE_BUCKETS];
static vnode_t vn_lru = {           /* dummy head of the LRU list, self-linked */
    .lru_prev = &vn_lru,
    .lru_next = &vn_lru,
};
static int vn_cache_count;
static spinlock_t vn_lock = SPINLOCK_INIT;

static uint32_t vn_key(struct mount *m, int ino) {
    uint32_t h = (uint32_t)(uintptr_t)m;
    h ^= (uint32_t)ino;
    h *= 2654435761u;
    h ^= h >> 13;
    h *= 2654435761u;
    h ^= h >> 16;
    return h & (VNODE_CACHE_BUCKETS - 1);
}

static vnode_t *vn_hash_find(struct mount *m, int ino) {
    vnode_t *vp = vn_hash[vn_key(m, ino)];
    while (vp) {
        if (vp->mount == m && vp->ino == ino)
            return vp;
        vp = vp->vnext;
    }
    return NULL;
}

static void vn_lru_unlink(vnode_t *vp) {
    vp->lru_prev->lru_next = vp->lru_next;
    vp->lru_next->lru_prev = vp->lru_prev;
}

static void vn_lru_push(vnode_t *vp) {
    vp->lru_next = vn_lru.lru_next;
    vp->lru_prev = &vn_lru;
    vn_lru.lru_next->lru_prev = vp;
    vn_lru.lru_next = vp;
}

/* Destroy a vnode that no other code can see (not in hash, refcount 0). */
static void vn_destroy(vnode_t *vp) {
    if (vp->ops && vp->ops->close)
        vp->ops->close(vp);
    kfree(vp);
}

/* Drop cache entries until at most VNODE_CACHE_MAX remain.  Called
 * with vn_lock held; only evicts refcount-0 entries from the LRU tail. */
static void vn_cache_trim(void) {
    while (vn_cache_count > VNODE_CACHE_MAX) {
        vnode_t *victim = vn_lru.lru_prev;
        if (victim == &vn_lru)
            break;
        if (victim->refcount != 0) {
            vn_lru_unlink(victim);
            victim->cached = 0;
            continue;
        }
        uint32_t b = vn_key(victim->mount, victim->ino);
        vnode_t **pp = &vn_hash[b];
        while (*pp) {
            if (*pp == victim) {
                *pp = victim->vnext;
                break;
            }
            pp = &(*pp)->vnext;
        }
        vn_lru_unlink(victim);
        victim->in_hash = 0;
        victim->cached = 0;
        vn_cache_count--;
        vn_destroy(victim);
    }
}

vnode_t *vnode_cache_get(struct mount *m, int ino) {
    uint32_t flags;
    spin_lock_irqsave(&vn_lock, &flags);

    vnode_t *vp = vn_hash_find(m, ino);
    if (vp) {
        if (vp->cached)
            vn_lru_unlink(vp);
        vp->cached = 0;
        vp->refcount++;
        spin_unlock_irqrestore(&vn_lock, flags);
        return vp;
    }

    spin_unlock_irqrestore(&vn_lock, flags);

    vnode_t *fresh = vnode_alloc();
    if (!fresh)
        return NULL;
    fresh->mount = m;
    fresh->ino = ino;
    fresh->refcount = 1;
    return fresh;
}

int vnode_cache_commit(vnode_t *vp) {
    uint32_t flags;
    spin_lock_irqsave(&vn_lock, &flags);

    if (vn_hash_find(vp->mount, vp->ino)) {
        /* Another looker won the race — drop our copy. */
        spin_unlock_irqrestore(&vn_lock, flags);
        vnode_put(vp);
        return -1;
    }

    uint32_t b = vn_key(vp->mount, vp->ino);
    vp->vnext = vn_hash[b];
    vn_hash[b] = vp;
    vp->in_hash = 1;
    vn_cache_count++;
    vn_cache_trim();

    spin_unlock_irqrestore(&vn_lock, flags);
    return 0;
}

void vnode_cache_invalidate(struct mount *m, int ino) {
    uint32_t flags;
    spin_lock_irqsave(&vn_lock, &flags);

    vnode_t *vp = vn_hash_find(m, ino);
    if (!vp) {
        spin_unlock_irqrestore(&vn_lock, flags);
        return;
    }

    uint32_t b = vn_key(m, ino);
    vnode_t **pp = &vn_hash[b];
    while (*pp) {
        if (*pp == vp) {
            *pp = vp->vnext;
            break;
        }
        pp = &(*pp)->vnext;
    }
    vp->in_hash = 0;
    if (vp->cached) {
        vn_lru_unlink(vp);
        vp->cached = 0;
    }
    vn_cache_count--;

    if (vp->refcount == 0) {
        spin_unlock_irqrestore(&vn_lock, flags);
        vn_destroy(vp);
        return;
    }
    vp->flags |= VDOOMED;
    spin_unlock_irqrestore(&vn_lock, flags);
}

void vnode_cache_flush_mount(struct mount *m) {
    uint32_t flags;
    spin_lock_irqsave(&vn_lock, &flags);

    for (int b = 0; b < VNODE_CACHE_BUCKETS; b++) {
        vnode_t **pp = &vn_hash[b];
        while (*pp) {
            vnode_t *vp = *pp;
            if (vp->mount != m) {
                pp = &vp->vnext;
                continue;
            }
            *pp = vp->vnext;
            vp->in_hash = 0;
            if (vp->cached) {
                vn_lru_unlink(vp);
                vp->cached = 0;
            }
            vn_cache_count--;
            if (vp->refcount == 0) {
                spin_unlock_irqrestore(&vn_lock, flags);
                vn_destroy(vp);
                spin_lock_irqsave(&vn_lock, &flags);
            } else {
                vp->flags |= VDOOMED;
            }
        }
    }
    spin_unlock_irqrestore(&vn_lock, flags);
}


/* Basic vnode lifecycle */
vnode_t *vnode_alloc(void) {
    vnode_t *vp = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vp) return NULL;
    memset(vp, 0, sizeof(vnode_t));
    vp->refcount = 0;
    vp->lock = SPINLOCK_INIT;
    return vp;
}

void vnode_ref(vnode_t *vp) {
    if (!vp) return;
    uint32_t flags;
    spin_lock_irqsave(&vp->lock, &flags);
    vp->refcount++;
    spin_unlock_irqrestore(&vp->lock, flags);
}

void vnode_unref(vnode_t *vp) {
    if (!vp) return;
    uint32_t flags;
    spin_lock_irqsave(&vp->lock, &flags);
    vp->refcount--;
    int doit = (vp->refcount <= 0);
    spin_unlock_irqrestore(&vp->lock, flags);
    if (!doit)
        return;

    /* Last reference dropped. */
    if (!vp->in_hash || (vp->flags & VDOOMED)) {
        vn_destroy(vp);
        return;
    }

    /* Return the vnode to the LRU cache for reuse.  The refcount drop
     * happens under vp->lock, but the LRU push is guarded by vn_lock;
     * between the two a concurrent vnode_cache_get (or vnode_ref) may
     * have re-armed the vnode (refcount 0->1).  Re-check the refcount
     * under vn_lock so a live vnode is never pushed onto the free list
     * (double-push would corrupt the LRU list). */
    uint32_t lflags;
    spin_lock_irqsave(&vn_lock, &lflags);
    if (!vp->cached && vp->refcount <= 0) {
        vp->cached = 1;
        vn_lru_push(vp);
    }
    vn_cache_trim();
    spin_unlock_irqrestore(&vn_lock, lflags);
}

vnode_t *vnode_get(vnode_t *vp) {
    vnode_ref(vp);
    return vp;
}

void vnode_put(vnode_t *vp) {
    vnode_unref(vp);
}
