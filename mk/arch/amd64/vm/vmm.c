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


#include "vmm.h"
#include "debug.h"
#include "panic.h"
#include "spinlock.h"
#include "memory.h"
#include "string.h"
#include "isr.h"
#include "personality.h"
#include "scheduler.h"
#include "thread.h"
#include "cpu.h"

/* amd64 4-level paging:
 *
 *   PML4[511]  → PDP (kernel half; shared by every process directory)
 *     PDP[510] → PD_shared:
 *       PD[0..31]   2MB huge pages: phys (i*2MB) → KERNEL_BASE + i*2MB
 *                   (direct map of the first 64MB)
 *       PD[32..]    heap 0xFFFFFFFF90000000..0xFFFFFFFFA0000000
 *                   (4KB pages mapped on demand)
 *     PDP[511] → PD (LAPIC/IOAPIC at 0xFFFFFFFFE0000000, TEMP_VADDR)
 *   PML4[0]   → PDP[0] → same PD_shared[0..31]: identity map 0..64MB
 *
 * Process directories copy every present PML4 entry from kernel_pml4,
 * so the whole kernel half (direct map, heap, temp slot, MMIO) is
 * shared and visible in every address space.
 */

#define TEMP_VADDR      0xFFFFFFFFFFFFF000ULL
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PTE_HUGE        (1ULL << 7)
#define IDENTITY_MAP_SIZE 0x4000000ULL   /* 64MB */

static pml4_t *kernel_pml4;
/* Per-CPU active PML4 (Phase 12): each CPU runs its own CR3 and the
 * fault/walk paths must use that CPU's directory. */
static pml4_t *current_pml4[CPU_MAX];
static pdp_t *kernel_low_pdp;

static inline void invlpg(uintptr_t addr) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void switch_cr3(uintptr_t dir) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(dir) : "memory");
}

static inline uintptr_t read_cr2(void) {
    uintptr_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

/* === Page table walk helpers === */

static pdp_t *get_pdp(pml4_t *pml4, uint64_t virt, int create) {
    size_t idx = (virt >> 39) & 0x1FF;
    if (pml4->entries[idx] & VMM_PRESENT)
        return (pdp_t *)(uintptr_t)(pml4->entries[idx] & PTE_ADDR_MASK);
    if (!create)
        return NULL;
    pdp_t *pdp = (pdp_t *)pmm_alloc_page();
    if (!pdp)
        return NULL;
    memset(pdp, 0, sizeof(*pdp));
    pml4->entries[idx] = (uintptr_t)pdp | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return pdp;
}

static pd_t *get_pd(pdp_t *pdp, uint64_t virt, int create) {
    size_t idx = (virt >> 30) & 0x1FF;
    if (pdp->entries[idx] & VMM_PRESENT)
        return (pd_t *)(uintptr_t)(pdp->entries[idx] & PTE_ADDR_MASK);
    if (!create)
        return NULL;
    pd_t *pd = (pd_t *)pmm_alloc_page();
    if (!pd)
        return NULL;
    memset(pd, 0, sizeof(*pd));
    pdp->entries[idx] = (uintptr_t)pd | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return pd;
}

static pt_t *get_pt(pd_t *pd, uint64_t virt, int create) {
    size_t idx = (virt >> 21) & 0x1FF;
    if (pd->entries[idx] & VMM_PRESENT)
        return (pt_t *)(uintptr_t)(pd->entries[idx] & PTE_ADDR_MASK);
    if (!create)
        return NULL;
    pt_t *pt = (pt_t *)pmm_alloc_page();
    if (!pt)
        return NULL;
    memset(pt, 0, sizeof(*pt));
    pd->entries[idx] = (uintptr_t)pt | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return pt;
}

/* Split a 2MB huge page into 512 4KB pages. Used when a 4KB mapping
 * must land inside a huge page (e.g. a user ELF segment linked into
 * the identity-mapped low 64MB). Sub-pages keep the huge page's flags
 * (identity stays supervisor-only); the PD entry points at the new
 * page table and gains VMM_USER so user mode can traverse it. */
static int split_huge_page(pd_t *pd, size_t pd_idx) {
    uint64_t huge = pd->entries[pd_idx];
    uint64_t phys_base = huge & PTE_ADDR_MASK;
    uint32_t leaf_flags = (uint32_t)(huge & 0xFFF) & ~PTE_HUGE;

    pt_t *pt = (pt_t *)pmm_alloc_page();
    if (!pt)
        return -1;
    memset(pt, 0, sizeof(*pt));
    for (int i = 0; i < 512; i++)
        pt->entries[i] = (phys_base + (uint64_t)i * PAGE_SIZE) | leaf_flags;

    pd->entries[pd_idx] = (uint64_t)(uintptr_t)pt | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return 0;
}

/* Leaf entry for virt (handles 2MB huge pages); NULL if unmapped */
static uint64_t *walk_leaf(pml4_t *pml4, uint64_t virt) {
    pdp_t *pdp = get_pdp(pml4, virt, 0);
    if (!pdp)
        return NULL;
    pd_t *pd = get_pd(pdp, virt, 0);
    if (!pd)
        return NULL;
    size_t pd_idx = (virt >> 21) & 0x1FF;
    if (pd->entries[pd_idx] & PTE_HUGE)
        return &pd->entries[pd_idx];
    pt_t *pt = get_pt(pd, virt, 0);
    if (!pt)
        return NULL;
    return &pt->entries[(virt >> 12) & 0x1FF];
}

void page_fault_handler(registers_t *r);

/* === VMM public API === */

void vmm_init(void) {
    kernel_pml4 = (pml4_t *)pmm_alloc_page();
    if (!kernel_pml4) {
        log_print(LOG_LEVEL_ERROR, "vmm: failed to alloc PML4\r\n");
        return;
    }
    memset(kernel_pml4, 0, sizeof(*kernel_pml4));

    pdp_t *pdp_high = (pdp_t *)pmm_alloc_page();
    pdp_t *pdp_low  = (pdp_t *)pmm_alloc_page();
    pd_t  *pd       = (pd_t *)pmm_alloc_page();
    if (!pdp_high || !pdp_low || !pd) {
        log_print(LOG_LEVEL_ERROR, "vmm: failed to alloc page tables\r\n");
        return;
    }
    memset(pdp_high, 0, sizeof(*pdp_high));
    memset(pdp_low, 0, sizeof(*pdp_low));
    memset(pd, 0, sizeof(*pd));

    /* Kernel half: PML4[511] → PDP[510] → PD_shared (direct map + heap) */
    kernel_pml4->entries[511] = (uintptr_t)pdp_high | VMM_PRESENT | VMM_WRITABLE;
    pdp_high->entries[510]    = (uintptr_t)pd | VMM_PRESENT | VMM_WRITABLE;

    /* Identity map: PML4[0] → PDP[0] → same PD_shared */
    kernel_pml4->entries[0] = (uintptr_t)pdp_low | VMM_PRESENT | VMM_WRITABLE;
    pdp_low->entries[0]     = (uintptr_t)pd | VMM_PRESENT | VMM_WRITABLE;
    kernel_low_pdp = pdp_low;

    /* First 64MB as 2MB huge pages: phys (i*2MB) identity AND at KERNEL_BASE */
    for (int i = 0; i < 32; i++) {
        pd->entries[i] = (uint64_t)(uintptr_t)((uint64_t)i * 0x200000ULL)
                       | VMM_PRESENT | VMM_WRITABLE | PTE_HUGE;
    }

    current_pml4[0] = kernel_pml4;   /* vmm_init runs on the BSP */
    switch_cr3((uintptr_t)kernel_pml4);
    log_printf(LOG_LEVEL_INFO, "vmm: kernel_pml4 phys=%p\r\n", (void *)kernel_pml4);

    register_interrupt_handler(14, page_fault_handler);

    log_print(LOG_LEVEL_INFO, "vmm: paging enabled (4-level)\r\n");
    log_print(LOG_LEVEL_INFO, "vmm: kernel mapped at 0xFFFFFFFF80000000\r\n");
    log_print(LOG_LEVEL_INFO, "vmm: identity map 0-64MB\r\n");
}

/* === Kernel heap (first-fit with headers, pages mapped on demand) === */

typedef struct heap_block {
    uint64_t size;
    struct heap_block *next;
} heap_block_t;

#define HEAP_BLOCK_FREE    1
#define HEAP_SIZE_MASK    (~1ULL)
#define HEAP_HEADER_SIZE   sizeof(heap_block_t)
#define HEAP_ALIGNMENT     16
#define HEAP_ALIGN(sz)     (((sz) + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1))
#define HEAP_MIN_BLOCK     (HEAP_ALIGN(HEAP_HEADER_SIZE + HEAP_ALIGNMENT))

static heap_block_t *heap_free_list;
static uint64_t heap_mapped_end;
static uint64_t heap_brk;
static spinlock_t heap_lock = SPINLOCK_INIT;

static int heap_map_until(uint64_t addr) {
    while (heap_mapped_end < addr) {
        void *phys = pmm_alloc_page();
        if (!phys) return -1;
        if (vmm_map_page(kernel_pml4, (uint64_t)(uintptr_t)phys, heap_mapped_end,
                         VMM_PRESENT | VMM_WRITABLE) < 0)
            return -1;
        heap_mapped_end += PAGE_SIZE;
    }
    return 0;
}

static void heap_coalesce(heap_block_t *b) {
    heap_block_t *next = (heap_block_t *)((uint8_t *)b + (b->size & HEAP_SIZE_MASK));
    if ((uint64_t)next < heap_brk && (next->size & HEAP_BLOCK_FREE)) {
        b->size = (b->size & HEAP_SIZE_MASK) + (next->size & HEAP_SIZE_MASK);
        b->next = next->next;
    }
}

/* Retract heap_brk past free blocks that end exactly at brk, then unmap
 * and return to the PMM every page above the new page-aligned brk.
 * Caller must hold heap_lock. */
static void heap_shrink(void) {
    for (;;) {
        heap_block_t *prev = NULL;
        heap_block_t *cur = heap_free_list;
        heap_block_t *tail = NULL;
        heap_block_t *tail_prev = NULL;

        while (cur) {
            if ((uint8_t *)cur + (cur->size & HEAP_SIZE_MASK) == (uint8_t *)heap_brk) {
                tail = cur;
                tail_prev = prev;
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        if (!tail)
            break;

        heap_brk = (uint64_t)(uintptr_t)tail;
        if (tail_prev)
            tail_prev->next = tail->next;
        else
            heap_free_list = tail->next;
    }

    uint64_t keep = (heap_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t floor = HEAP_START + HEAP_INITIAL_PAGES * PAGE_SIZE;
    if (keep < floor)
        keep = floor;

    while (heap_mapped_end > keep) {
        uint64_t addr = heap_mapped_end - PAGE_SIZE;
        uint64_t phys = vmm_get_physical(kernel_pml4, addr);
        vmm_unmap_page(kernel_pml4, addr);
        if (phys)
            pmm_free_page((void *)(uintptr_t)phys);
        heap_mapped_end -= PAGE_SIZE;
    }
}

void *kmalloc(uint32_t size) {
    if (size == 0)
        return NULL;

    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

    uint64_t need = HEAP_HEADER_SIZE + HEAP_ALIGN(size);
    if (need < HEAP_MIN_BLOCK)
        need = HEAP_MIN_BLOCK;

    heap_block_t *prev = NULL;
    heap_block_t *b = heap_free_list;

    while (b) {
        uint64_t block_size = b->size & HEAP_SIZE_MASK;
        if (block_size >= need) {
            uint64_t remaining = block_size - need;
            if (remaining >= HEAP_MIN_BLOCK) {
                b->size = need | 0;
                heap_block_t *split = (heap_block_t *)((uint8_t *)b + need);
                split->size = remaining | HEAP_BLOCK_FREE;
                split->next = b->next;
                if (prev)
                    prev->next = split;
                else
                    heap_free_list = split;
            } else {
                b->size = block_size | 0;
                if (prev)
                    prev->next = b->next;
                else
                    heap_free_list = b->next;
            }
            spin_unlock_irqrestore(&heap_lock, flags);
            return (void *)((uint8_t *)b + HEAP_HEADER_SIZE);
        }
        prev = b;
        b = b->next;
    }

    uint64_t addr = heap_brk;
    uint64_t new_brk = addr + need;
    if (new_brk >= HEAP_END) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    if (heap_map_until(new_brk) < 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    heap_brk = new_brk;

    heap_block_t *block = (heap_block_t *)(uintptr_t)addr;
    block->size = need | 0;
    block->next = NULL;
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)((uint8_t *)block + HEAP_HEADER_SIZE);
}

void *kcalloc(uint32_t count, uint32_t size) {
    /* Guard the multiplication: an overflowed total would allocate a
     * tiny buffer while every caller assumes the full count*size. */
    if (count != 0 && size != 0 && size > 0xFFFFFFFFu / count)
        return NULL;
    uint32_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (uint32_t i = 0; i < total; i++)
            p[i] = 0;
    }
    return ptr;
}

void kfree(void *addr) {
    if (!addr)
        return;

    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

    heap_block_t *b = (heap_block_t *)((uint8_t *)addr - HEAP_HEADER_SIZE);
    uint64_t block_size = b->size & HEAP_SIZE_MASK;

    b->size = block_size | HEAP_BLOCK_FREE;

    heap_block_t *prev = NULL;
    heap_block_t *cur = heap_free_list;

    while (cur && (uint64_t)cur < (uint64_t)b) {
        prev = cur;
        cur = cur->next;
    }

    b->next = cur;
    if (prev)
        prev->next = b;
    else
        heap_free_list = b;

    heap_coalesce(b);
    if (prev && (uint64_t)prev + (prev->size & HEAP_SIZE_MASK) == (uint64_t)b)
        heap_coalesce(prev);

    heap_shrink();

    spin_unlock_irqrestore(&heap_lock, flags);
}

void vmm_init_heap(void) {
    heap_free_list = NULL;
    heap_mapped_end = HEAP_START;
    heap_brk = HEAP_START;

    heap_map_until(HEAP_START + HEAP_INITIAL_PAGES * PAGE_SIZE);

    log_printf(LOG_LEVEL_INFO, "vmm: heap initialized at 0x%lx\r\n", HEAP_START);
}

page_directory_t *vmm_create_directory(void) {
    pml4_t *dir = (pml4_t *)pmm_alloc_page();
    if (!dir)
        return NULL;
    memset(dir, 0, sizeof(*dir));

    /* Share the kernel high half with the kernel */
    for (int i = 256; i < 512; i++) {
        if (kernel_pml4->entries[i] & VMM_PRESENT)
            dir->entries[i] = kernel_pml4->entries[i];
    }

    /* Low half: the kernel's identity mappings (kernel code executes at
     * identity addresses, and MMIO such as AHCI at 0xFFB00000 is mapped
     * identity) must be reachable while running with this directory in
     * CR3, e.g. when an IRQ or syscall is handled in user context.
     * PML4[0] gets a per-process PDP: entries 0/1 are private PDs for
     * user space (identity huge pages 0-64MB copied as supervisor),
     * the rest mirror the kernel's low-half entries. */
    pdp_t *pdp = (pdp_t *)pmm_alloc_page();
    pd_t *pd_user0 = (pd_t *)pmm_alloc_page();
    pd_t *pd_user1 = (pd_t *)pmm_alloc_page();
    if (!pdp || !pd_user0 || !pd_user1) {
        if (pdp)
            pmm_free_page(pdp);
        if (pd_user0)
            pmm_free_page(pd_user0);
        if (pd_user1)
            pmm_free_page(pd_user1);
        pmm_free_page(dir);
        return NULL;
    }
    memset(pdp, 0, sizeof(*pdp));
    memset(pd_user0, 0, sizeof(*pd_user0));
    memset(pd_user1, 0, sizeof(*pd_user1));

    for (int i = 0; i < 32; i++) {
        pd_user0->entries[i] = (uint64_t)(uintptr_t)((uint64_t)i * 0x200000ULL)
                             | VMM_PRESENT | VMM_WRITABLE | PTE_HUGE;
    }
    pdp->entries[0] = (uintptr_t)pd_user0 | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    pdp->entries[1] = (uintptr_t)pd_user1 | VMM_PRESENT | VMM_WRITABLE | VMM_USER;

    for (int i = 2; i < 256; i++) {
        if (kernel_low_pdp && (kernel_low_pdp->entries[i] & VMM_PRESENT))
            pdp->entries[i] = kernel_low_pdp->entries[i];
    }

    dir->entries[0] = (uintptr_t)pdp | VMM_PRESENT | VMM_WRITABLE | VMM_USER;

    return dir;
}

void vmm_switch_directory(page_directory_t *dir) {
    if (!dir)
        return;
    current_pml4[cpu_current()->id] = dir;
    switch_cr3((uintptr_t)dir);
}

static int vmm_dir_in_use(page_directory_t *dir) {
    for (unsigned i = 0; i < CPU_MAX; i++) {
        if (current_pml4[i] == dir)
            return 1;
    }
    return 0;
}

void vmm_free_directory(page_directory_t *dir) {
    if (!dir)
        return;
    if (dir == kernel_pml4 || vmm_dir_in_use(dir))
        return;

    for (int pml4i = 0; pml4i < 512; pml4i++) {
        if (!(dir->entries[pml4i] & VMM_PRESENT))
            continue;
        if (dir->entries[pml4i] == kernel_pml4->entries[pml4i])
            continue;  /* shared kernel half / identity map */

        pdp_t *pdp = (pdp_t *)(uintptr_t)(dir->entries[pml4i] & PTE_ADDR_MASK);
        for (int pdpi = 0; pdpi < 512; pdpi++) {
            if (!(pdp->entries[pdpi] & VMM_PRESENT))
                continue;
            if (kernel_low_pdp && pdpi != 0 && pdpi != 1 &&
                pdp->entries[pdpi] == kernel_low_pdp->entries[pdpi])
                continue;  /* mirrored kernel low-half mapping (shared) */
            if (pdp->entries[pdpi] & PTE_HUGE)
                continue;
            pd_t *pd = (pd_t *)(uintptr_t)(pdp->entries[pdpi] & PTE_ADDR_MASK);
            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(pd->entries[pdi] & VMM_PRESENT))
                    continue;
                if (pd->entries[pdi] & PTE_HUGE)
                    continue;
                pt_t *pt = (pt_t *)(uintptr_t)(pd->entries[pdi] & PTE_ADDR_MASK);
                for (int pti = 0; pti < 512; pti++) {
                    uint64_t pte = pt->entries[pti];
                    if ((pte & VMM_PRESENT) && (pte & VMM_USER) &&
                        !(pte & VMM_COW))
                        /* COW pages are shared with another process
                         * (e.g. the parent's text); freeing them here
                         * would free memory the parent still uses. */
                        pmm_free_page((void *)(uintptr_t)(pte & PTE_ADDR_MASK));
                }
                pmm_free_page((void *)(uintptr_t)(pd->entries[pdi] & PTE_ADDR_MASK));
            }
            pmm_free_page((void *)(uintptr_t)(pdp->entries[pdpi] & PTE_ADDR_MASK));
        }
        pmm_free_page((void *)(uintptr_t)(dir->entries[pml4i] & PTE_ADDR_MASK));
    }

    pmm_free_page(dir);
}

page_directory_t *vmm_get_current_directory(void) {
    return current_pml4[cpu_current()->id];
}

/* Full local TLB flush (used by the IPI_TLB handler on remote CPUs). */
void vmm_tlb_reload_current(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

page_directory_t *vmm_get_kernel_directory(void) {
    return kernel_pml4;
}

int vmm_map_page(page_directory_t *dir, uint64_t phys, uint64_t virt, uint64_t flags) {
    if (!dir)
        return -1;
    pt_t *pt = NULL;
    pdp_t *pdp = get_pdp(dir, virt, 1);
    if (!pdp)
        return -1;
    pd_t *pd = get_pd(pdp, virt, 1);
    if (!pd)
        return -1;
    size_t pd_idx = (virt >> 21) & 0x1FF;
    if (pd->entries[pd_idx] & PTE_HUGE) {
        /* A 4KB mapping over a 2MB huge page: legal only in the user
         * half (per-process identity map). Split the huge page into
         * 4KB pages so the new mapping takes effect. The kernel's
         * shared huge pages (direct map) are never split. */
        if (dir == kernel_pml4 || ((virt >> 39) & 0x1FF) >= 256) {
            log_print(LOG_LEVEL_DEBUG, "vmm: map over huge page\r\n");
            return -1;
        }
        if (split_huge_page(pd, pd_idx) != 0)
            return -1;
    }
    pt = get_pt(pd, virt, 1);
    if (!pt)
        return -1;

    pt->entries[(virt >> 12) & 0x1FF] = (phys & ~0xFFFULL) | flags;
    invlpg(virt);
    return 0;
}

void vmm_unmap_page(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_leaf(dir, virt);
    if (!pte)
        return;
    *pte = 0;
    invlpg(virt);
}

uint64_t vmm_get_physical(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_leaf(dir, virt);
    if (!pte || !(*pte & VMM_PRESENT))
        return 0;
    return (*pte & PTE_ADDR_MASK) | (virt & 0xFFF);
}

int vmm_get_page_flags(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_leaf(dir, virt);
    if (!pte)
        return 0;
    return (int)(*pte & 0xFFF);
}

int vmm_is_page_present(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_leaf(dir, virt);
    return (pte && (*pte & VMM_PRESENT)) ? 1 : 0;
}

/* === User fault handling === */

static page_fault_handler_t user_fault_handler;

void vmm_register_fault_handler(page_fault_handler_t handler) {
    user_fault_handler = handler;
}

/* Copy-on-write: the faulting page is COW (read-only, VMM_COW set).
 * Allocate a private page, copy the shared contents through the temp
 * slot, and remap the faulting address writable. */
static int handle_cow_fault(pml4_t *pml4, uint64_t fault_addr) {
    uint64_t *pte = walk_leaf(pml4, fault_addr);
    if (!pte || !(*pte & VMM_PRESENT))
        return 0;
    if (!(*pte & VMM_COW))
        return 0;

    uint64_t old_phys = *pte & PTE_ADDR_MASK;
    void *new_page = pmm_alloc_page();
    if (!new_page)
        return 0;
    uint64_t new_phys = (uint64_t)(uintptr_t)new_page;

    void *old_va = vmm_temp_map(old_phys);
    if (!old_va) {
        pmm_free_page(new_page);
        return 0;
    }

    *pte = (new_phys & ~0xFFFULL) | ((*pte & 0xFFF) & ~(VMM_WRITABLE | VMM_COW)) | VMM_WRITABLE;
    invlpg(fault_addr);

    memcpy((void *)(uintptr_t)(fault_addr & ~0xFFFULL), old_va, PAGE_SIZE);
    vmm_temp_unmap();
    return 1;
}

void page_fault_handler(registers_t *r) {
    uint64_t fault_addr = read_cr2();
    uint32_t error_code = (uint32_t)r->err_code;
    pml4_t *pml4 = current_pml4[cpu_current()->id];

    /* COW break: a write to a copy-on-write page.  Handle this for
     * KERNEL faults too — the kernel legitimately writes user pages on
     * behalf of the process (copy_to_user, vnode ops into user
     * buffers), and a kernel-mode COW fault used to fall through to
     * panic().  handle_cow_fault checks the VMM_COW bit itself and
     * returns 0 for ordinary faults. */
    if ((error_code & 0x1) && (error_code & 0x2)) {
        if (handle_cow_fault(pml4, fault_addr))
            return;
    }

    if (error_code & 0x4) {
        /* Fault in user mode */
        if (user_fault_handler) {
            user_fault_handler(r, fault_addr, error_code);
            return;
        }

        log_printf(LOG_LEVEL_ERROR, "Page fault at 0x%lx (err=0x%x, rip=0x%lx) "
                     "in user mode — terminating process\r\n",
                     fault_addr, error_code, r->rip);
        if (proc_current && proc_exit) {
            struct proc *p = proc_current();
            if (p) {
                proc_exit(139, r);
                thread_exit(139);
                return;
            }
        }
        panic("Page Fault", r);
        return;
    }

    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    thread_t *ct = scheduler_current_thread();

    log_printf(LOG_LEVEL_ERROR, "PAGE FAULT at 0x%lx err=0x%x rip=0x%lx\r\n",
                 fault_addr, error_code, r->rip);

    log_printf(LOG_LEVEL_ERROR, "PF: type=%s%s%s rip_tag=%s\r\n",
                 (error_code & 0x1) ? "not-present" : "present",
                 (error_code & 0x2) ? "+write" : "+read",
                 (error_code & 0x10) ? "+fetch" : "",
                 (r->rip >= 0x100000 && r->rip < 0x117050) ? "TEXT" : "NOT-TEXT");
    log_printf(LOG_LEVEL_ERROR, "PF: cr3=0x%lx fault_mapped=%s rip_mapped=%s\r\n",
                 cr3,
                 walk_leaf(pml4, fault_addr) ? "yes" : "NO",
                 walk_leaf(pml4, r->rip) ? "yes" : "NO");
    log_printf(LOG_LEVEL_ERROR, "PF: cur tid=%u name='%s' state=%u kernel_rsp=0x%lx\r\n",
                 ct ? ct->tid : 0, ct ? ct->name : "?", ct ? ct->state : 0,
                 ct ? ct->kernel_rsp : 0);

    uint64_t *rip_pte = walk_leaf(pml4, r->rip);
    if (rip_pte) {
        log_printf(LOG_LEVEL_ERROR, "PF: rip bytes: ");
        for (int i = 0; i < 8; i++)
            log_printf(LOG_LEVEL_ERROR, "%02x ", ((unsigned char *)(uintptr_t)r->rip)[i]);
        log_print(LOG_LEVEL_ERROR, "\r\n");
    } else {
        log_print(LOG_LEVEL_ERROR, "PF: rip page NOT mapped — executing unmapped memory!\r\n");
    }

    log_printf(LOG_LEVEL_ERROR, "PF: rsp=0x%lx rbp=0x%lx", r->rsp, r->rbp);
    if (r->rbp >= 0x1000) {
        uint64_t *rp = walk_leaf(pml4, r->rbp);
        uint64_t *rpr = walk_leaf(pml4, r->rbp + 8);
        if (rp && rpr) {
            log_printf(LOG_LEVEL_ERROR, " rbp[ret]=0x%lx", *(uint64_t *)(uintptr_t)(r->rbp + 8));
        }
    }
    log_print(LOG_LEVEL_ERROR, "\r\n");

    panic("Page Fault", r);
}

/* === COW fork === */

/* Get (or create) the child's page table at the given 4K index */
static uint64_t *child_pte(page_directory_t *child, uint64_t virt) {
    pdp_t *pdp = get_pdp(child, virt, 1);
    if (!pdp)
        return NULL;
    pd_t *pd = get_pd(pdp, virt, 1);
    if (!pd)
        return NULL;
    size_t pd_idx = (virt >> 21) & 0x1FF;
    if (pd->entries[pd_idx] & PTE_HUGE) {
        if (split_huge_page(pd, pd_idx) != 0)
            return NULL;
    }
    pt_t *pt = get_pt(pd, virt, 1);
    if (!pt)
        return NULL;
    return &pt->entries[(virt >> 12) & 0x1FF];
}

void vmm_fork_cow_pages(page_directory_t *parent_dir, page_directory_t *child_dir) {
    if (!parent_dir || !child_dir)
        return;

    /* User half only: PML4 indices 0..255. Shared entries (identity
     * map, kernel tables) are skipped — only private user pages COW. */
    for (int pml4i = 0; pml4i < 256; pml4i++) {
        if (!(parent_dir->entries[pml4i] & VMM_PRESENT))
            continue;
        if (parent_dir->entries[pml4i] == kernel_pml4->entries[pml4i])
            continue;

        pdp_t *parent_pdp = (pdp_t *)(uintptr_t)(parent_dir->entries[pml4i] & PTE_ADDR_MASK);
        for (int pdpi = 0; pdpi < 512; pdpi++) {
            if (!(parent_pdp->entries[pdpi] & VMM_PRESENT))
                continue;
            if (parent_pdp->entries[pdpi] & PTE_HUGE)
                continue;

            pd_t *parent_pd = (pd_t *)(uintptr_t)(parent_pdp->entries[pdpi] & PTE_ADDR_MASK);
            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(parent_pd->entries[pdi] & VMM_PRESENT))
                    continue;
                if (parent_pd->entries[pdi] & PTE_HUGE)
                    continue;

                pt_t *parent_pt = (pt_t *)(uintptr_t)(parent_pd->entries[pdi] & PTE_ADDR_MASK);
                uint64_t virt_base = ((uint64_t)pml4i << 39)
                                   | ((uint64_t)pdpi << 30)
                                   | ((uint64_t)pdi << 21);

                for (int pti = 0; pti < 512; pti++) {
                    uint64_t pte = parent_pt->entries[pti];
                    if (!(pte & VMM_PRESENT))
                        continue;
                    if (!(pte & VMM_USER))
                        continue;

                    uint64_t *child_entry = child_pte(child_dir, virt_base | ((uint64_t)pti << 12));
                    if (!child_entry)
                        continue;

                    /* Pages already COW (marked by an earlier fork) keep
                     * their COW PTE — the new child just shares the same
                     * read-only page.  Skipping them would leave the
                     * child's fresh pd_user0 huge pages untouched, so the
                     * child could not even read its own text. */
                    uint64_t flags = (pte & 0xFFF) & ~VMM_WRITABLE;
                    flags |= VMM_COW;

                    parent_pt->entries[pti] = (pte & ~0xFFFULL) | flags;
                    invlpg(virt_base | ((uint64_t)pti << 12));
                    *child_entry = parent_pt->entries[pti];
                }
            }
        }
    }

    /* The parent's pages just became read-only, but only THIS cpu's
     * TLB was invalidated (invlpg above).  A sibling thread of the
     * same address space running on another CPU could keep writing
     * through its stale writable TLB entry — silently corrupting the
     * COW protocol.  Force a TLB flush on every other CPU. */
    tlb_flush_others();
}

void vmm_clear_user_pages(page_directory_t *dir) {
    if (!dir)
        return;

    for (int pml4i = 0; pml4i < 256; pml4i++) {
        if (!(dir->entries[pml4i] & VMM_PRESENT))
            continue;
        if (dir->entries[pml4i] == kernel_pml4->entries[pml4i])
            continue;

        pdp_t *pdp = (pdp_t *)(uintptr_t)(dir->entries[pml4i] & PTE_ADDR_MASK);
        for (int pdpi = 0; pdpi < 512; pdpi++) {
            if (!(pdp->entries[pdpi] & VMM_PRESENT))
                continue;
            if (pdp->entries[pdpi] & PTE_HUGE)
                continue;

            pd_t *pd = (pd_t *)(uintptr_t)(pdp->entries[pdpi] & PTE_ADDR_MASK);
            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(pd->entries[pdi] & VMM_PRESENT))
                    continue;
                if (pd->entries[pdi] & PTE_HUGE)
                    continue;

                pt_t *pt = (pt_t *)(uintptr_t)(pd->entries[pdi] & PTE_ADDR_MASK);
                for (int pti = 0; pti < 512; pti++) {
                    if (pt->entries[pti] & VMM_USER)
                        pt->entries[pti] = 0;
                }
            }
        }
    }

    switch_cr3((uintptr_t)dir);
}

/* === Temp mapping (one 4K slot PER CPU, at TEMP_VADDR - id*PAGE_SIZE)
 * All slots live in the same page table (PD index 511 of the kernel
 * PDP[511]), mapped into kernel_pml4 — every process shares the kernel
 * half, so the slots are visible in whatever address space is active.
 * A single shared slot let two CPUs servicing faults concurrently
 * overwrite each other's mapping and copy the WRONG physical page
 * into a process (cross-process data corruption), so each CPU owns a
 * private slot.  Callers still must not sleep between map/unmap. */

#define TEMP_SLOTS_MAX 32

static uint64_t temp_vaddr_for(unsigned cpu_id) {
    if (cpu_id >= TEMP_SLOTS_MAX)
        cpu_id = 0;
    return TEMP_VADDR - (uint64_t)cpu_id * PAGE_SIZE;
}

static unsigned temp_cpu_slot(void) {
    struct cpu *c = cpu_current();
    return c ? c->id : 0;
}

void *vmm_temp_map(uint64_t phys) {
    uint64_t va = temp_vaddr_for(temp_cpu_slot());
    if (vmm_map_page(kernel_pml4, phys, va, VMM_PRESENT | VMM_WRITABLE) < 0)
        return NULL;
    return (void *)(uintptr_t)va;
}

void vmm_temp_unmap(void) {
    uint64_t va = temp_vaddr_for(temp_cpu_slot());
    vmm_unmap_page(kernel_pml4, va);
    invlpg(va);
}

/* === User copy helpers ===
 *
 * Every helper validates the FULL range up front: each covered page
 * must exist and carry VMM_USER.  Without this a user-supplied kernel
 * pointer would give ring 0 an arbitrary read/write primitive, and a
 * partially unmapped buffer would fault in kernel mode and panic. */

int user_range_ok(const void *uaddr, uint32_t size, int write) {
    if (size == 0)
        return 1;
    if (!uaddr)
        return 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;

    /* Overflow-safe upper bound check against the user/kernel split.
     * (size is 32-bit, so only the address terms need guarding.) */
    if (addr >= USER_STACK_TOP || addr + size > USER_STACK_TOP)
        return 0;

    page_directory_t *dir = vmm_get_current_directory();
    uint64_t first_page = addr & ~0xFFFULL;
    uint64_t last_page = (addr + size - 1) & ~0xFFFULL;
    for (uint64_t page = first_page; ; page += PAGE_SIZE) {
        int flags = vmm_get_page_flags(dir, page);
        if (!(flags & VMM_PRESENT)) return 0;
        if (!(flags & VMM_USER)) return 0;
        /* COW counts as writable: the pending write breaks it (the
         * page-fault handler now services COW breaks from kernel mode). */
        if (write && !(flags & (VMM_WRITABLE | VMM_COW))) return 0;
        if (page == last_page)
            break;
    }
    return 1;
}

int copy_from_user(void *dst, const void *user_src, uint32_t size) {
    if (size == 0) return 0;
    if (!user_range_ok(user_src, size, 0)) return -1;

    for (uint32_t i = 0; i < size; i++)
        ((uint8_t *)dst)[i] = ((const uint8_t *)(uintptr_t)user_src)[i];
    return 0;
}

int copy_to_user(void *user_dst, const void *src, uint32_t size) {
    if (size == 0) return 0;
    if (!user_range_ok(user_dst, size, 1)) return -1;

    for (uint32_t i = 0; i < size; i++)
        ((uint8_t *)(uintptr_t)user_dst)[i] = ((const uint8_t *)src)[i];
    return 0;
}

int strncpy_from_user(char *dst, const char *user_src, uint32_t max_len) {
    if (max_len == 0) return -1;
    if (!user_src) return -1;
    uint64_t addr = (uint64_t)(uintptr_t)user_src;
    if (addr >= USER_STACK_TOP) return -1;
    uint64_t avail = USER_STACK_TOP - addr;
    if (max_len > avail) max_len = (uint32_t)avail;
    if (max_len == 0) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    uint64_t cur_page = addr & ~0xFFFULL;
    int flags = vmm_get_page_flags(dir, cur_page);
    if (!(flags & VMM_PRESENT) || !(flags & VMM_USER)) return -1;

    /* Walk byte by byte and stop at the NUL — a string that ends early
     * must not require pages BEYOND its terminator to be mapped. */
    for (uint32_t i = 0; i < max_len; i++) {
        char c = ((const char *)(uintptr_t)user_src)[i];
        dst[i] = c;
        if (c == '\0') return (int)(i + 1);
        if (((addr + i + 1) & ~0xFFFULL) != cur_page) {
            cur_page = (addr + i + 1) & ~0xFFFULL;
            flags = vmm_get_page_flags(dir, cur_page);
            if (!(flags & VMM_PRESENT) || !(flags & VMM_USER)) return -1;
        }
    }
    return -1;
}

