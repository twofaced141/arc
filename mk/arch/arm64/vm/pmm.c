/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 */

#include <stdint.h>
#include "memory.h"
#include "pmm.h"
#include "platform.h"
#include "spinlock.h"
#include "string.h"
#include "debug.h"

extern uint64_t _kernel_start;
extern uint64_t _kernel_end;

static uint8_t *bitmap;
static uint32_t total_pages;
static uint32_t free_pages;
static int ready;

static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1u << (page % 8));
}

static inline void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1u << (page % 8));
}

static inline int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1u << (page % 8));
}

void pmm_init(void) {
    uint64_t ram_base = arm64_ram_base;
    uint64_t ram_size = arm64_ram_size;
    if (ram_size == 0) {
        ram_base = ARM64_DEFAULT_RAM_BASE;
        ram_size = ARM64_DEFAULT_RAM_SIZE;
    }

    uint64_t kstart = (uint64_t)&_kernel_start;
    uint64_t kend = (uint64_t)&_kernel_end;
    kend = (kend + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (ram_base == 0)
        ram_base = ARM64_DEFAULT_RAM_BASE;

    total_pages = (uint32_t)(ram_size / PAGE_SIZE);
    if (total_pages == 0) {
        ready = 0;
        return;
    }

    uint32_t bitmap_bytes = (total_pages + 7) / 8;
    uint32_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (bitmap_pages == 0)
        bitmap_pages = 1;

    uint64_t bitmap_phys = kend;
    /* Ensure bitmap fits inside RAM */
    if (bitmap_phys < ram_base)
        bitmap_phys = ram_base;
    if (bitmap_phys + (uint64_t)bitmap_pages * PAGE_SIZE > ram_base + ram_size) {
        /* Not enough space after kernel: try to place bitmap at ram_base gap */
        if (kstart > ram_base && (kstart - ram_base) >= (uint64_t)bitmap_pages * PAGE_SIZE) {
            bitmap_phys = ram_base;
        } else {
            debug_print("pmm: not enough RAM for bitmap\n");
            ready = 0;
            return;
        }
    }

    bitmap = (uint8_t *)(uintptr_t)bitmap_phys;
    /* Mark all as used initially */
    memset(bitmap, 0xFF, (size_t)bitmap_pages * PAGE_SIZE);

    uint32_t kstart_idx = 0;
    uint32_t kend_idx = 0;
    uint32_t bstart_idx = (uint32_t)((bitmap_phys - ram_base) / PAGE_SIZE);
    uint32_t bend_idx = bstart_idx + bitmap_pages;

    if (kstart >= ram_base && kstart < ram_base + ram_size)
        kstart_idx = (uint32_t)((kstart - ram_base) / PAGE_SIZE);
    if (kend >= ram_base && kend <= ram_base + ram_size)
        kend_idx = (uint32_t)((kend - ram_base) / PAGE_SIZE);
    else
        kend_idx = (uint32_t)((bitmap_phys - ram_base) / PAGE_SIZE);

    free_pages = 0;
    for (uint32_t i = 0; i < total_pages; i++) {
        int reserved = 0;
        if (i >= kstart_idx && i < kend_idx)
            reserved = 1;
        if (i >= bstart_idx && i < bend_idx)
            reserved = 1;
        /* Kernel heap static region __heap_start..__heap_end is inside kernel image,
         * already covered by kend. DTB lives inside RAM but we treat it as free
         * for now — it sits below kend in identity map and is copied early. */
        if (!reserved) {
            bitmap_clear(i);
            free_pages++;
        }
    }

    ready = 1;
    log_printf(LOG_LEVEL_INFO,
               "pmm: RAM 0x%lx size %lu MB, %u pages, bitmap %u pages at 0x%lx free %u\r\n",
               (unsigned long)ram_base,
               (unsigned long)(ram_size / (1024*1024)),
               total_pages, bitmap_pages,
               (unsigned long)bitmap_phys, free_pages);
}

void *pmm_alloc_page(void) {
    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (!ready || free_pages == 0) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
    }
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(uintptr_t)(arm64_ram_base + (uint64_t)i * PAGE_SIZE);
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void *pmm_alloc_pages(uint32_t count) {
    if (count == 0)
        return NULL;
    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (!ready || free_pages < count) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
    }
    for (uint32_t i = 0; i + count <= total_pages; i++) {
        uint32_t j;
        for (j = 0; j < count; j++) {
            if (bitmap_test(i + j))
                break;
        }
        if (j == count) {
            for (uint32_t k = 0; k < count; k++) {
                bitmap_set(i + k);
                free_pages--;
            }
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(uintptr_t)(arm64_ram_base + (uint64_t)i * PAGE_SIZE);
        }
        /* skip ahead past the used page to avoid O(n^2) */
        if (j != 0)
            i += j;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void pmm_free_page(void *page) {
    pmm_free_pages(page, 1);
}

void pmm_free_pages(void *addr, uint32_t count) {
    if (!addr || count == 0)
        return;
    uint64_t phys = (uint64_t)(uintptr_t)addr;
    if (phys < arm64_ram_base || phys >= arm64_ram_base + arm64_ram_size)
        return;
    if ((phys & (PAGE_SIZE - 1)) != 0)
        return;
    uint32_t start = (uint32_t)((phys - arm64_ram_base) / PAGE_SIZE);
    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = start + i;
        if (idx >= total_pages)
            break;
        if (bitmap_test(idx)) {
            bitmap_clear(idx);
            free_pages++;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint32_t pmm_get_free_pages(void) {
    if (!ready) return 0;
    /* free_pages is updated under lock but reading without lock is racy;
     * still return approximate to match x86 API. */
    return free_pages;
}

uint32_t pmm_get_total_pages(void) {
    return total_pages;
}
