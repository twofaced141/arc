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


#include "vm_object.h"
#include "vmm.h"
#include "string.h"
#include "debug.h"
#include "memory.h"

/* errno */
#define EAGAIN  11

/* ================================================================
 * vm_map.c — VM map operations
 *
 * A vm_map is a task's address space: an array of vm_entry ranges
 * sorted by start address.  The entry array is dynamically allocated
 * and grows by 2× when exhausted.
 * ================================================================ */

#define VM_MAP_INIT_CAPACITY    64
#define VM_MAP_MAX_CAPACITY     65536

/* ---- Internal helpers ---- */

static int entry_overlaps(const vm_entry_t *e, uint64_t start, uint64_t end) {
    return (e->start < end) && (e->end > start);
}

static void entries_shift_right(vm_map_t *map, int index) {
    for (int i = map->entry_count; i > index; i--) {
        map->entries[i] = map->entries[i - 1];
    }
}

static void entries_shift_left(vm_map_t *map, int index) {
    for (int i = index; i < map->entry_count - 1; i++) {
        map->entries[i] = map->entries[i + 1];
    }
    map->entry_count--;
}

/* ---- Grow the entry array (double capacity) ---- */
static int vm_map_grow(vm_map_t *map) {
    uint32_t new_cap = map->max_entries < VM_MAP_INIT_CAPACITY
                       ? VM_MAP_INIT_CAPACITY
                       : map->max_entries * 2;
    if (new_cap > VM_MAP_MAX_CAPACITY)
        return -1;

    vm_entry_t *new_entries = (vm_entry_t *)kmalloc(new_cap * sizeof(vm_entry_t));
    if (!new_entries)
        return -1;

    memcpy(new_entries, map->entries, map->entry_count * sizeof(vm_entry_t));
    kfree(map->entries);
    map->entries = new_entries;
    map->max_entries = (int)new_cap;
    return 0;
}

/* Ensure at least `needed` free slots after the current entry_count */
static int vm_map_ensure_capacity(vm_map_t *map, int extra) {
    while (map->entry_count + extra > map->max_entries) {
        if (vm_map_grow(map) < 0)
            return -1;
    }
    return 0;
}

/* ================================================================
 * Public API
 * ================================================================ */

int vm_map_init(vm_map_t *map, struct page_directory *pml4) {
    if (!map || !pml4)
        return -1;

    map->entries = (vm_entry_t *)kmalloc(VM_MAP_INIT_CAPACITY * sizeof(vm_entry_t));
    if (!map->entries)
        return -1;

    memset(map->entries, 0, VM_MAP_INIT_CAPACITY * sizeof(vm_entry_t));
    map->max_entries = VM_MAP_INIT_CAPACITY;
    map->entry_count = 0;
    map->pml4 = pml4;
    map->lock = SPINLOCK_INIT;

    debug_print("vm_map: initialized (dynamic entries)\r\n");
    return 0;
}

void vm_map_destroy(vm_map_t *map) {
    if (!map)
        return;

    uint32_t flags;
    spin_lock_irqsave(&map->lock, &flags);

    for (int i = 0; i < map->entry_count; i++) {
        vm_entry_t *e = &map->entries[i];
        if (e->obj) {
            vm_object_release(e->obj);
            e->obj = NULL;
        }
    }
    map->entry_count = 0;

    kfree(map->entries);
    map->entries = NULL;
    map->max_entries = 0;

    spin_unlock_irqrestore(&map->lock, flags);
    debug_print("vm_map: destroyed\r\n");
}

int vm_map_map(vm_map_t *map, uint64_t addr, vm_object_t *obj,
               uint64_t obj_offset, uint64_t size, uint32_t prot,
               uint32_t inherit) {
    if (!map || !obj || size == 0)
        return -1;

    if (addr & (PAGE_SIZE - 1))
        return -1;  /* addr must be page-aligned */
    if (obj_offset & (PAGE_SIZE - 1))
        return -1;  /* obj_offset must be page-aligned */
    if (size & (PAGE_SIZE - 1))
        return -1;  /* size must be page-aligned */

    uint64_t end = addr + size;
    if (end < addr)
        return -1;  /* overflow */

    uint32_t flags;
    spin_lock_irqsave(&map->lock, &flags);

    /* Check for overlap with existing entries */
    for (int i = 0; i < map->entry_count; i++) {
        if (entry_overlaps(&map->entries[i], addr, end)) {
            spin_unlock_irqrestore(&map->lock, flags);
            debug_printf("vm_map: overlap at 0x%lx-0x%lx with entry %d (0x%lx-0x%lx)\r\n",
                         addr, end, i, map->entries[i].start, map->entries[i].end);
            return -1;
        }
    }

    /* Grow if at capacity */
    if (vm_map_ensure_capacity(map, 1) < 0) {
        spin_unlock_irqrestore(&map->lock, flags);
        debug_print("vm_map: max capacity reached\r\n");
        return -1;
    }

    /* Find insertion point to keep entries sorted by start address */
    int insert_idx = map->entry_count;
    for (int i = 0; i < map->entry_count; i++) {
        if (map->entries[i].start > addr) {
            insert_idx = i;
            break;
        }
    }

    /* Shift entries right to make room */
    entries_shift_right(map, insert_idx);

    /* Fill in the new entry */
    vm_entry_t *e = &map->entries[insert_idx];
    e->start = addr;
    e->end = end;
    e->obj = vm_object_retain(obj);
    e->obj_offset = obj_offset;
    e->prot = prot;
    e->inherit = inherit;

    map->entry_count++;

    spin_unlock_irqrestore(&map->lock, flags);

    debug_printf("vm_map: mapped 0x%lx-0x%lx (obj=%p, off=0x%lx, prot=0x%x, inherit=%u)\r\n",
                 addr, end, (void *)obj, obj_offset, prot, inherit);
    return 0;
}

int vm_map_unmap(vm_map_t *map, uint64_t addr, uint64_t size) {
    if (!map || size == 0)
        return -1;

    uint64_t end = addr + size;
    if (end < addr)
        return -1;

    uint32_t flags;
    spin_lock_irqsave(&map->lock, &flags);

    int removed = 0;
    for (int i = 0; i < map->entry_count; ) {
        vm_entry_t *e = &map->entries[i];

        if (!entry_overlaps(e, addr, end)) {
            i++;
            continue;
        }

        /* Four overlap cases:
         * (a) Full overwrite: [addr, end) completely covers [e->start, e->end)
         * (b) Left partial:   [addr, end) overlaps left part of entry
         * (c) Right partial:  [addr, end) overlaps right part of entry
         * (d) Split:          [addr, end) is strictly inside entry -> split into two
         */

        if (addr <= e->start && end >= e->end) {
            /* Case (a): full overwrite - remove entire entry */
            if (e->obj)
                vm_object_release(e->obj);
            entries_shift_left(map, i);
            removed++;
            /* Don't increment i - next entry shifted into this index */
        }
        else if (addr <= e->start && end < e->end) {
            /* Case (b): left partial - shrink entry from left */
            uint64_t new_start = end;
            uint64_t offset_delta = new_start - e->start;
            e->obj_offset += offset_delta;
            e->start = new_start;
            i++;
        }
        else if (addr > e->start && end >= e->end) {
            /* Case (c): right partial - shrink entry from right */
            e->end = addr;
            i++;
        }
        else if (addr > e->start && end < e->end) {
            /* Case (d): split - create new entry for right fragment */
            if (vm_map_ensure_capacity(map, 1) < 0) {
                spin_unlock_irqrestore(&map->lock, flags);
                debug_print("vm_map: unmap split would exceed max capacity\r\n");
                return -1;
            }

            vm_entry_t right_fragment = *e;
            right_fragment.start = end;
            right_fragment.obj_offset += (end - e->start);
            vm_object_retain(right_fragment.obj);

            /* Shrink left fragment */
            e->end = addr;

            /* Insert right fragment after current entry */
            entries_shift_right(map, i + 1);
            map->entries[i + 1] = right_fragment;
            map->entry_count++;

            i += 2;  /* Skip both fragments */
            removed++;
        }
        else {
            i++;
        }
    }

    spin_unlock_irqrestore(&map->lock, flags);

    debug_printf("vm_map: unmapped 0x%lx-0x%lx, removed %d entries\r\n", addr, end, removed);
    return 0;
}

int vm_map_protect(vm_map_t *map, uint64_t addr, uint64_t size, uint32_t prot) {
    if (!map || size == 0)
        return -1;

    uint64_t end = addr + size;
    if (end < addr)
        return -1;

    uint32_t flags;
    spin_lock_irqsave(&map->lock, &flags);

    int changed = 0;
    for (int i = 0; i < map->entry_count; ) {
        vm_entry_t *e = &map->entries[i];

        if (!entry_overlaps(e, addr, end)) {
            i++;
            continue;
        }

        if (addr <= e->start && end >= e->end) {
            /* Full cover: just update protection */
            e->prot = prot;
            changed++;
            i++;
        }
        else if (addr <= e->start && end < e->end) {
            /* Left partial: split off left part with new prot */
            if (vm_map_ensure_capacity(map, 1) < 0) {
                spin_unlock_irqrestore(&map->lock, flags);
                return -1;
            }

            vm_entry_t right_fragment = *e;
            right_fragment.start = end;
            right_fragment.obj_offset += (end - e->start);
            vm_object_retain(right_fragment.obj);

            e->end = end;
            e->prot = prot;

            entries_shift_right(map, i + 1);
            map->entries[i + 1] = right_fragment;
            map->entry_count++;

            i += 2;
            changed++;
        }
        else if (addr > e->start && end >= e->end) {
            /* Right partial: split off right part with new prot */
            if (vm_map_ensure_capacity(map, 1) < 0) {
                spin_unlock_irqrestore(&map->lock, flags);
                return -1;
            }

            vm_entry_t left_fragment = *e;
            left_fragment.end = addr;
            vm_object_retain(left_fragment.obj);

            e->start = addr;
            e->obj_offset += (addr - left_fragment.start);
            e->prot = prot;

            entries_shift_right(map, i + 1);
            map->entries[i + 1] = left_fragment;
            map->entry_count++;

            i += 2;
            changed++;
        }
        else if (addr > e->start && end < e->end) {
            /* Split into three: left (old prot), middle (new prot), right (old prot) */
            if (vm_map_ensure_capacity(map, 2) < 0) {
                spin_unlock_irqrestore(&map->lock, flags);
                return -1;
            }

            /* Save original bounds before modifying e */
            uint64_t entry_start = e->start;
            uint64_t entry_off = e->obj_offset;

            /* Right fragment (copy before modifying e) */
            vm_entry_t right_fragment = *e;
            right_fragment.start = end;
            right_fragment.obj_offset += (end - entry_start);
            vm_object_retain(right_fragment.obj);

            /* Left fragment at i (shrink, keep old prot) */
            e->end = addr;

            /* Middle entry (new prot) */
            vm_entry_t middle;
            middle.start = addr;
            middle.end = end;
            middle.obj = vm_object_retain(e->obj);
            middle.obj_offset = entry_off + (addr - entry_start);
            middle.prot = prot;
            middle.inherit = e->inherit;

            /* Insert middle at i+1, right at i+2 */
            entries_shift_right(map, i + 1);
            entries_shift_right(map, i + 2);
            map->entries[i + 1] = middle;
            map->entries[i + 2] = right_fragment;
            map->entry_count += 2;

            i += 3;
            changed++;
        }
        else {
            i++;
        }
    }

    spin_unlock_irqrestore(&map->lock, flags);

    debug_printf("vm_map: protect 0x%lx-0x%lx prot=0x%x, changed %d entries\r\n",
                 addr, end, prot, changed);
    return 0;
}

vm_entry_t *vm_map_find_entry(vm_map_t *map, uint64_t addr) {
    if (!map || map->entry_count <= 0)
        return NULL;

    /* Binary search over the sorted-by-start entry array.
     * Find the rightmost entry with start <= addr, then verify addr < end.
     * O(log n) instead of the previous O(n) scan. */
    int lo = 0, hi = map->entry_count - 1;
    int best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (map->entries[mid].start <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (best >= 0 && addr < map->entries[best].end)
        return &map->entries[best];
    return NULL;
}

int vm_map_resolve_fault(vm_map_t *map, uint64_t fault_addr, int write_fault,
                         uint64_t *out_phys) {
    if (!map || !out_phys)
        return -1;

    uint32_t flags;
    spin_lock_irqsave(&map->lock, &flags);

    vm_entry_t *entry = vm_map_find_entry(map, fault_addr);
    if (!entry) {
        spin_unlock_irqrestore(&map->lock, flags);
        debug_printf("vm_map: fault at 0x%lx - no entry found (SIGSEGV)\r\n", fault_addr);
        return -1;  /* SIGSEGV */
    }

    /* Check protection */
    uint32_t required_prot = write_fault ? VM_PROT_WRITE : VM_PROT_READ;
    if (!(entry->prot & required_prot)) {
        spin_unlock_irqrestore(&map->lock, flags);
        debug_printf("vm_map: fault at 0x%lx - protection violation (prot=0x%x, need=0x%x)\r\n",
                     fault_addr, entry->prot, required_prot);
        return -1;  /* SIGSEGV */
    }

    /* Compute offset within the object */
    uint64_t offset = (fault_addr - entry->start) + entry->obj_offset;
    offset &= ~(PAGE_SIZE - 1);  /* page-align */

    vm_page_t page;
    int ret = entry->obj->fault(entry->obj, offset, &page, write_fault);

    if (ret == 0) {
        *out_phys = page.phys_addr;
        spin_unlock_irqrestore(&map->lock, flags);
        return 0;
    }
    else if (ret == -EAGAIN) {
        /* User pager fault - caller will block and retry */
        spin_unlock_irqrestore(&map->lock, flags);
        return -EAGAIN;
    }
    else {
        /* Other error from fault handler */
        spin_unlock_irqrestore(&map->lock, flags);
        debug_printf("vm_map: fault handler returned %d for 0x%lx\r\n", ret, fault_addr);
        return -1;
    }
}
