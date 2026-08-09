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


#include "task.h"
#include "vmm.h"
#include "debug.h"
#include "string.h"

/* ================================================================
 * cspace.c — C-space: capability slot management
 *
 * Each task has a C-space whose slot table and free bitmap are
 * dynamically allocated and grow by 2× when exhausted (up to
 * CSPACE_MAX_SLOTS).
 * ================================================================ */

static int cspace_grow(cspace_t *cs);

int cspace_init(cspace_t *cs) {
    if (!cs) return -1;

    cs->max_slots    = CSPACE_DEFAULT_SLOTS;
    cs->bitmap_words = CSPACE_DEFAULT_SLOTS / CSPACE_WORD_BITS;

    cs->slots = (cslot_t *)kmalloc(cs->max_slots * sizeof(cslot_t));
    if (!cs->slots) {
        cs->max_slots = 0;
        return -1;
    }

    cs->free_bitmap = (uint32_t *)kmalloc(cs->bitmap_words * sizeof(uint32_t));
    if (!cs->free_bitmap) {
        kfree(cs->slots);
        cs->slots = NULL;
        cs->max_slots = 0;
        return -1;
    }

    memset(cs->slots, 0, cs->max_slots * sizeof(cslot_t));
    memset(cs->free_bitmap, 0, cs->bitmap_words * sizeof(uint32_t));
    cs->lock = SPINLOCK_INIT;

    /* Slot 0 is reserved (null/invalid capability) */
    cs->free_bitmap[0] |= (1u << 0);

    return 0;
}

void cspace_destroy(cspace_t *cs) {
    if (!cs) return;
    if (cs->slots)      kfree(cs->slots);
    if (cs->free_bitmap) kfree(cs->free_bitmap);
    cs->slots       = NULL;
    cs->free_bitmap  = NULL;
    cs->max_slots    = 0;
    cs->bitmap_words = 0;
}

/* ---- Grow capacity 2× (internally locked, caller must NOT hold cs->lock) ---- */
static int cspace_grow(cspace_t *cs) {
    int new_max = cs->max_slots * 2;
    if (new_max > CSPACE_MAX_SLOTS)
        return -1;

    int new_words = new_max / CSPACE_WORD_BITS;

    cslot_t *new_slots = (cslot_t *)kmalloc(new_max * sizeof(cslot_t));
    if (!new_slots) return -1;

    uint32_t *new_bitmap = (uint32_t *)kmalloc(new_words * sizeof(uint32_t));
    if (!new_bitmap) {
        kfree(new_slots);
        return -1;
    }

    /* Copy existing slots */
    memcpy(new_slots, cs->slots, cs->max_slots * sizeof(cslot_t));

    /* Build new bitmap: mark all new slots as free, copy existing bits */
    memcpy(new_bitmap, cs->free_bitmap, cs->bitmap_words * sizeof(uint32_t));
    memset(new_bitmap + cs->bitmap_words, 0,
           (new_words - cs->bitmap_words) * sizeof(uint32_t));

    kfree(cs->slots);
    kfree(cs->free_bitmap);

    cs->slots       = new_slots;
    cs->free_bitmap = new_bitmap;
    cs->max_slots   = new_max;
    cs->bitmap_words = new_words;

    return 0;
}

/* ---- Internal: find first zero bit in the bitmap ---- */
static int bitmap_alloc(uint32_t *bitmap, int nwords) {
    for (int w = 0; w < nwords; w++) {
        if (bitmap[w] == 0xFFFFFFFF) continue;  /* word full */
        for (int b = 0; b < 32; b++) {
            if (!(bitmap[w] & (1u << b))) {
                bitmap[w] |= (1u << b);
                return w * 32 + b;
            }
        }
    }
    return -1;  /* all slots used */
}

static void bitmap_free(uint32_t *bitmap, int slot) {
    int w = slot / 32;
    int b = slot % 32;
    bitmap[w] &= ~(1u << b);
}

int cspace_alloc_slot(cspace_t *cs, uint32_t type, uint32_t rights, uint64_t object_id) {
    if (!cs) return -1;

    uint32_t flags;
    spin_lock_irqsave(&cs->lock, &flags);

    int slot = bitmap_alloc(cs->free_bitmap, cs->bitmap_words);
    if (slot < 0) {
        /* Out of slots — try to grow */
        spin_unlock_irqrestore(&cs->lock, flags);

        if (cspace_grow(cs) < 0)
            return -1;

        spin_lock_irqsave(&cs->lock, &flags);
        slot = bitmap_alloc(cs->free_bitmap, cs->bitmap_words);
        if (slot < 0) {
            spin_unlock_irqrestore(&cs->lock, flags);
            return -1;
        }
    }

    cs->slots[slot].object_id = object_id;
    cs->slots[slot].type      = type;
    cs->slots[slot].rights    = rights;
    cs->slots[slot].in_use    = 1;

    spin_unlock_irqrestore(&cs->lock, flags);
    return slot;
}

int cspace_free_slot(cspace_t *cs, int slot) {
    if (!cs || slot < 0 || !cs->slots || slot >= cs->max_slots) return -1;

    uint32_t flags;
    spin_lock_irqsave(&cs->lock, &flags);

    if (!cs->slots[slot].in_use) {
        spin_unlock_irqrestore(&cs->lock, flags);
        return -1;
    }

    cs->slots[slot].object_id = 0;
    cs->slots[slot].type      = 0;
    cs->slots[slot].rights    = 0;
    cs->slots[slot].in_use    = 0;
    bitmap_free(cs->free_bitmap, slot);

    spin_unlock_irqrestore(&cs->lock, flags);
    return 0;
}

cslot_t *cspace_lookup(cspace_t *cs, int slot) {
    if (!cs || slot < 0 || !cs->slots || slot >= cs->max_slots) return NULL;
    if (!cs->slots[slot].in_use) return NULL;
    return &cs->slots[slot];
}

int cspace_move(cspace_t *from, cspace_t *to, int slot) {
    if (!from || !to || slot < 0 || !from->slots || slot >= from->max_slots) return -1;

    uint32_t fflags, tflags;

    /* Lock both: always take lower address first to avoid deadlock */
    if ((uintptr_t)from < (uintptr_t)to) {
        spin_lock_irqsave(&from->lock, &fflags);
        spin_lock_irqsave(&to->lock, &tflags);
    } else {
        spin_lock_irqsave(&to->lock, &tflags);
        spin_lock_irqsave(&from->lock, &fflags);
    }

    if (!from->slots[slot].in_use) {
        spin_unlock_irqrestore(&from->lock, fflags);
        spin_unlock_irqrestore(&to->lock, tflags);
        return -1;
    }

    /* Find a free slot in destination */
    int dst_slot = bitmap_alloc(to->free_bitmap, to->bitmap_words);
    if (dst_slot < 0) {
        /* Try to grow destination, then retry */
        spin_unlock_irqrestore(&from->lock, fflags);
        spin_unlock_irqrestore(&to->lock, tflags);

        if (cspace_grow(to) < 0)
            return -1;

        /* Re-acquire locks and retry */
        if ((uintptr_t)from < (uintptr_t)to) {
            spin_lock_irqsave(&from->lock, &fflags);
            spin_lock_irqsave(&to->lock, &tflags);
        } else {
            spin_lock_irqsave(&to->lock, &tflags);
            spin_lock_irqsave(&from->lock, &fflags);
        }

        if (!from->slots[slot].in_use) {
            spin_unlock_irqrestore(&from->lock, fflags);
            spin_unlock_irqrestore(&to->lock, tflags);
            return -1;
        }

        dst_slot = bitmap_alloc(to->free_bitmap, to->bitmap_words);
        if (dst_slot < 0) {
            spin_unlock_irqrestore(&from->lock, fflags);
            spin_unlock_irqrestore(&to->lock, tflags);
            return -1;
        }
    }

    /* Move: copy to destination, clear source */
    to->slots[dst_slot] = from->slots[slot];

    from->slots[slot].object_id = 0;
    from->slots[slot].type      = 0;
    from->slots[slot].rights    = 0;
    from->slots[slot].in_use    = 0;
    bitmap_free(from->free_bitmap, slot);

    spin_unlock_irqrestore(&from->lock, fflags);
    spin_unlock_irqrestore(&to->lock, tflags);

    return dst_slot;
}
