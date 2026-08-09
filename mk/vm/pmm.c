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


/* This file is x86 (i386/amd64) only.
 * arm64 has its own pmm in mk/arch/arm64/vm/pmm.c */
#ifndef __aarch64__

#include "pmm.h"
#include "debug.h"
#include "vmm.h"
#include "string.h"

/* The bootloader (amd64 boot.s) identity-maps only the first
 * BOOTSTRAP_BYTES of physical memory, so the dynamic PMM bitmap is
 * bump-allocated inside that window: it is directly writable from
 * pmm_init() onward and stays mapped after vmm_init() (which keeps the
 * first 64MB identity-mapped).
 *
 * The bitmap itself covers total_pages = top of RAM / PAGE_SIZE, so
 * there is no compile-time cap on how much RAM the kernel sees; only
 * the capacity of this low-memory window (16MB minus the kernel image,
 * enough for hundreds of GB) limits it. */
#define BOOTSTRAP_BYTES  (16 * 1024 * 1024)
#define BOOTSTRAP_PAGES  (BOOTSTRAP_BYTES / PAGE_SIZE)

static uint8_t bootstrap_bitmap[BOOTSTRAP_BYTES / 8 / PAGE_SIZE];
static uint8_t *bitmap;
static uint32_t total_pages;
static uint32_t free_pages;
static uint16_t *refcounts;

extern uint32_t _kernel_end;

static inline void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

static inline void bmap_set(uint32_t page) {
    bootstrap_bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bmap_clear(uint32_t page) {
    bootstrap_bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bmap_test(uint32_t page) {
    return bootstrap_bitmap[page / 8] & (1 << (page % 8));
}

/* Mark pages [start, end) used in the bootstrap bitmap (clipped). */
static void bmap_reserve(uint32_t start, uint32_t end) {
    if (end > BOOTSTRAP_PAGES)
        end = BOOTSTRAP_PAGES;
    for (uint32_t i = start; i < end; i++)
        bmap_set(i);
}

void pmm_init(multiboot2_info_t *mboot) {
    uint64_t mem_end = 0;
    uint32_t i;

    /* Bootstrap bitmap: everything used by default, then free the
     * e820-available pages (clipped to the identity-mapped window). */
    for (i = 0; i < sizeof(bootstrap_bitmap); i++)
        bootstrap_bitmap[i] = 0xFF;

    multiboot2_tag_t *tag = multiboot2_first_tag(mboot);
    while (tag->type != MULTIBOOT_TAG_END) {
        if (tag->type == MULTIBOOT_TAG_MMAP) {
            multiboot2_tag_mmap_t *mtag = (multiboot2_tag_mmap_t *)tag;
            uint8_t *end = (uint8_t *)tag + tag->size;

            for (uint8_t *p = (uint8_t *)mtag->entries;
                 p < end;
                 p += mtag->entry_size) {
                multiboot2_mmap_entry_t *e = (multiboot2_mmap_entry_t *)p;
                if (e->type != MULTIBOOT_MEMORY_AVAILABLE)
                    continue;

                uint64_t top = e->addr + e->len;
                if (top > mem_end)
                    mem_end = top;

                uint32_t start_page = (e->addr + PAGE_SIZE - 1) / PAGE_SIZE;
                uint32_t end_page = top / PAGE_SIZE;
                if (start_page >= BOOTSTRAP_PAGES)
                    continue;
                if (end_page > BOOTSTRAP_PAGES)
                    end_page = BOOTSTRAP_PAGES;
                for (i = start_page; i < end_page; i++)
                    bmap_clear(i);
            }
        }
        tag = multiboot2_next_tag(tag);
    }

    total_pages = mem_end / PAGE_SIZE;

    /* Reserve the kernel image */
    uint32_t kernel_end_page = ((uintptr_t)&_kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    bmap_reserve(0, kernel_end_page);

    /* Reserve multiboot2 modules and the boot info structure */
    for (tag = multiboot2_first_tag(mboot); tag->type != MULTIBOOT_TAG_END; tag = multiboot2_next_tag(tag)) {
        if (tag->type == MULTIBOOT_TAG_MODULE) {
            multiboot2_tag_module_t *mod = (multiboot2_tag_module_t *)tag;
            bmap_reserve(mod->mod_start / PAGE_SIZE,
                         (mod->mod_end + PAGE_SIZE - 1) / PAGE_SIZE);
        }
    }
    bmap_reserve((uintptr_t)mboot / PAGE_SIZE,
                 ((uintptr_t)mboot + mboot->total_size + PAGE_SIZE - 1) / PAGE_SIZE);

    /* Bump-allocate the real bitmap from the lowest free run of pages */
    uint32_t bitmap_pages = (total_pages / 8 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (bitmap_pages == 0)
        bitmap_pages = 1;

    uint32_t bstart = BOOTSTRAP_PAGES;
    uint32_t run = 0;
    for (i = 0; i < BOOTSTRAP_PAGES; i++) {
        if (!bmap_test(i)) {
            if (run == 0)
                bstart = i;
            if (++run == bitmap_pages)
                break;
        } else {
            run = 0;
        }
    }

    if (run != bitmap_pages) {
        /* Not enough low memory for a full bitmap: track what fits. */
        uint32_t max_run = 0;
        uint32_t r = 0;
        for (i = 0; i < BOOTSTRAP_PAGES; i++) {
            if (!bmap_test(i)) {
                if (++r > max_run)
                    max_run = r;
            } else {
                r = 0;
            }
        }
        uint64_t capped = (uint64_t)max_run * 8 * PAGE_SIZE;
        log_printf(LOG_LEVEL_WARN,
                   "pmm: bitmap needs %u pages, capping RAM at %llu MB\r\n",
                   bitmap_pages,
                   (unsigned long long)(capped / (1024 * 1024)));
        total_pages = (uint32_t)(capped / PAGE_SIZE);
        bitmap_pages = (total_pages / 8 + PAGE_SIZE - 1) / PAGE_SIZE;
        if (bitmap_pages == 0)
            bitmap_pages = 1;

        run = 0;
        bstart = BOOTSTRAP_PAGES;
        for (i = 0; i < BOOTSTRAP_PAGES; i++) {
            if (!bmap_test(i)) {
                if (run == 0)
                    bstart = i;
                if (++run == bitmap_pages)
                    break;
            } else {
                run = 0;
            }
        }
    }

    bitmap = (uint8_t *)(uintptr_t)((uint64_t)bstart * PAGE_SIZE);
    memset(bitmap, 0xFF, (size_t)bitmap_pages * PAGE_SIZE);

    /* Free the e820-available ranges in the real bitmap */
    tag = multiboot2_first_tag(mboot);
    while (tag->type != MULTIBOOT_TAG_END) {
        if (tag->type == MULTIBOOT_TAG_MMAP) {
            multiboot2_tag_mmap_t *mtag = (multiboot2_tag_mmap_t *)tag;
            uint8_t *end = (uint8_t *)tag + tag->size;

            for (uint8_t *p = (uint8_t *)mtag->entries;
                 p < end;
                 p += mtag->entry_size) {
                multiboot2_mmap_entry_t *e = (multiboot2_mmap_entry_t *)p;
                if (e->type != MULTIBOOT_MEMORY_AVAILABLE)
                    continue;

                uint32_t start_page = (e->addr + PAGE_SIZE - 1) / PAGE_SIZE;
                uint32_t end_page = (e->addr + e->len) / PAGE_SIZE;
                if (start_page >= total_pages)
                    continue;
                if (end_page > total_pages)
                    end_page = total_pages;

                for (i = start_page; i < end_page; i++) {
                    if (bitmap_test(i)) {
                        bitmap_clear(i);
                        free_pages++;
                    }
                }
            }
        }
        tag = multiboot2_next_tag(tag);
    }

    /* Reserve the kernel image */
    for (i = 0; i < kernel_end_page && i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
        }
    }

    /* Reserve the bitmap itself */
    uint32_t bitmap_start_page = (uintptr_t)bitmap / PAGE_SIZE;
    uint32_t bitmap_end_page = bitmap_start_page + bitmap_pages;
    for (i = bitmap_start_page; i < bitmap_end_page && i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
        }
    }

    /* Reserve multiboot2 modules */
    for (tag = multiboot2_first_tag(mboot); tag->type != MULTIBOOT_TAG_END; tag = multiboot2_next_tag(tag)) {
        if (tag->type == MULTIBOOT_TAG_MODULE) {
            multiboot2_tag_module_t *mod = (multiboot2_tag_module_t *)tag;
            uint32_t mod_start_page = mod->mod_start / PAGE_SIZE;
            uint32_t mod_end_page = (mod->mod_end + PAGE_SIZE - 1) / PAGE_SIZE;
            for (i = mod_start_page; i < mod_end_page && i < total_pages; i++) {
                if (!bitmap_test(i)) {
                    bitmap_set(i);
                    free_pages--;
                }
            }
        }
    }

    /* Reserve the multiboot2 info structure */
    uintptr_t mboot_start_page = (uintptr_t)mboot / PAGE_SIZE;
    uintptr_t mboot_end_page = ((uintptr_t)mboot + mboot->total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uintptr_t pi = mboot_start_page; pi < mboot_end_page && pi < total_pages; pi++) {
        if (!bitmap_test((uint32_t)pi)) {
            bitmap_set((uint32_t)pi);
            free_pages--;
        }
    }

    log_printf(LOG_LEVEL_INFO,
               "pmm: RAM %llu MB, %u pages tracked, bitmap %u pages at 0x%lx\r\n",
               (unsigned long long)((uint64_t)total_pages * PAGE_SIZE / (1024 * 1024)),
               total_pages, bitmap_pages, (unsigned long)(uintptr_t)bitmap);
}

void *pmm_alloc_page(void) {
    if (free_pages == 0)
        return (void *)0;

    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            if (!bitmap_test(i))
                continue;
            free_pages--;
            if (refcounts)
                refcounts[i] = 1;
            return (void *)(uintptr_t)((uint64_t)i * PAGE_SIZE);
        }
    }
    return (void *)0;
}

void *pmm_alloc_pages(uint32_t count) {
    if (count == 0 || free_pages < count)
        return NULL;

    for (uint32_t i = 0; i <= total_pages - count; i++) {
        uint32_t j;
        for (j = 0; j < count; j++) {
            if (bitmap_test(i + j))
                break;
        }

        if (j == count) {
            for (uint32_t k = 0; k < count; k++) {
                bitmap_set(i + k);
                free_pages--;
                if (refcounts)
                    refcounts[i + k] = 1;
            }
            return (void *)(uintptr_t)((uint64_t)i * PAGE_SIZE);
        }
    }
    return NULL;
}

void pmm_free_page(void *page) {
    uintptr_t idx = (uintptr_t)page / PAGE_SIZE;
    if (idx < total_pages && bitmap_test(idx)) {
        bitmap_clear(idx);
        free_pages++;
    }
}

void pmm_free_pages(void *addr, uint32_t count) {
    uintptr_t start = (uintptr_t)addr / PAGE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        if (start + i < total_pages && bitmap_test(start + i)) {
            bitmap_clear(start + i);
            free_pages++;
        }
    }
}

uint32_t pmm_get_free_pages(void) {
    return free_pages;
}

uint32_t pmm_get_total_pages(void) {
    return total_pages;
}

void pmm_refcount_init(void) {
    /* Stub: refcounting not yet needed in microkernel */
}

#endif /* !__aarch64__ */
