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
#include "string.h"
#include "pmm.h"
#include "memory.h"
#include "debug.h"
#include "fdt.h"

/* PCI ECAM base discovered from FDT (extern from main.c). */
extern uint64_t pci_ecam_base;
extern uint64_t pci_ecam_size;

#define L0_IDX(v)  (((v) >> 39) & 0x1FF)
#define L1_IDX(v)  (((v) >> 30) & 0x1FF)
#define L2_IDX(v)  (((v) >> 21) & 0x1FF)
#define L3_IDX(v)  (((v) >> 12) & 0x1FF)

#define DESC_VALID      (1ULL << 0)
#define DESC_TABLE      (1ULL << 1)
#define DESC_PAGE       (3ULL << 0)
#define ATTR_UXN        (1ULL << 54)
#define ATTR_PXN        (1ULL << 53)
#define ATTR_CONT       (1ULL << 52)
#define ATTR_DBM        (1ULL << 51)
#define ATTR_nG         (1ULL << 11)
#define ATTR_AF         (1ULL << 10)
#define ATTR_SH(n)      (((uint64_t)(n) & 3) << 8)
#define ATTR_AP_RO      (1ULL << 7)
#define ATTR_AP_USER    (1ULL << 6)
#define ATTR_ATTR_IDX(n) (((uint64_t)(n) & 7) << 2)
#define ADDR_MASK       0x0000FFFFFFFFF000ULL

#define SH_INNER        3
#define MT_NORMAL       0
#define MT_DEVICE       1

#define TCR_T0SZ(n)     ((64 - (n)) & 0x3F)
#define TCR_T1SZ(n)     (((64 - (n)) & 0x3F) << 16)
#define TCR_TG0_4KB     (0ULL << 14)
#define TCR_TG1_4KB     (2ULL << 30)
#define TCR_SH0_INNER   (3ULL << 12)
#define TCR_SH1_INNER   (3ULL << 28)
#define TCR_ORGN0_WBWA  (1ULL << 10)
#define TCR_IRGN0_WBWA  (1ULL << 8)
#define TCR_ORGN1_WBWA  (1ULL << 26)
#define TCR_IRGN1_WBWA  (1ULL << 24)
#define TCR_IPS_40BIT   (2ULL << 32)
#define TCR_AS16BIT     (1ULL << 36)

#define SCTLR_MMU       (1ULL << 0)
#define SCTLR_CACHE     (1ULL << 2)
#define SCTLR_I         (1ULL << 12)
#define SCTLR_SA0       (1ULL << 4)
#define SCTLR_SA        (1ULL << 3)

#define RAM_BASE        0x40000000ULL
#define RAM_SIZE        0x04000000ULL   /* 64MB */
#define RAM_END         (RAM_BASE + RAM_SIZE)

#define GICD_BASE       0x08000000ULL
#define GICC_BASE       0x08010000ULL
#define UART_BASE       0x09000000ULL

static page_directory_t *kernel_l1;
static page_directory_t *current_l1;
static page_directory_t *initial_l0;

static int mmu_on;

static void inv_page(uint64_t vaddr) {
    __asm__ __volatile__("dsb ishst\n\t"
                         "tlbi vae1is, %0\n\t"
                         "dsb ish\n\t"
                         "isb" : : "r"(vaddr >> 12) : "memory");
}

static page_directory_t *get_l1(page_directory_t *l0, uint64_t vaddr, int create) {
    uint64_t idx = L0_IDX(vaddr);
    if (!(l0->entries[idx] & DESC_VALID)) {
        if (!create) return NULL;
        page_directory_t *l1 = (page_directory_t *)pmm_alloc_page();
        if (!l1) return NULL;
        memset(l1, 0, sizeof(page_directory_t));
        l0->entries[idx] = (uint64_t)(uintptr_t)l1 | DESC_VALID | DESC_TABLE | ATTR_AF;
        __asm__ __volatile__("dsb sy\n\tisb");
    }
    return (page_directory_t *)(uintptr_t)(l0->entries[idx] & ADDR_MASK);
}

static page_directory_t *get_l2(page_directory_t *l1, uint64_t vaddr, int create) {
    uint64_t idx = L1_IDX(vaddr);
    if (!(l1->entries[idx] & DESC_VALID)) {
        if (!create) return NULL;
        page_directory_t *l2 = (page_directory_t *)pmm_alloc_page();
        if (!l2) return NULL;
        memset(l2, 0, sizeof(page_directory_t));
        l1->entries[idx] = (uint64_t)(uintptr_t)l2 | DESC_VALID | DESC_TABLE | ATTR_AF;
    }
    if (!(l1->entries[idx] & DESC_TABLE))
        return NULL;
    return (page_directory_t *)(uintptr_t)(l1->entries[idx] & ADDR_MASK);
}

static page_directory_t *get_l3(page_directory_t *l2, uint64_t vaddr, int create) {
    uint64_t idx = L2_IDX(vaddr);
    if (!(l2->entries[idx] & DESC_VALID)) {
        if (!create) return NULL;
        page_directory_t *l3 = (page_directory_t *)pmm_alloc_page();
        if (!l3) return NULL;
        memset(l3, 0, sizeof(page_directory_t));
        l2->entries[idx] = (uint64_t)(uintptr_t)l3 | DESC_VALID | DESC_TABLE | ATTR_AF;
    }
    if (!(l2->entries[idx] & DESC_TABLE))
        return NULL;
    return (page_directory_t *)(uintptr_t)(l2->entries[idx] & ADDR_MASK);
}

static uint64_t *walk_pt(page_directory_t *l0, uint64_t vaddr, int create) {
    page_directory_t *l1 = get_l1(l0, vaddr, create);
    if (!l1) return NULL;
    page_directory_t *l2 = get_l2(l1, vaddr, create);
    if (!l2) return NULL;
    page_directory_t *l3 = get_l3(l2, vaddr, create);
    if (!l3) return NULL;
    return &l3->entries[L3_IDX(vaddr)];
}

static uint64_t pte_flags(uint32_t flags) {
    uint64_t attr = DESC_PAGE | ATTR_AF;
    if (flags & VMM_CACHE_DISABLE)
        attr |= ATTR_ATTR_IDX(MT_DEVICE);
    else
        attr |= ATTR_ATTR_IDX(MT_NORMAL) | ATTR_SH(SH_INNER);
    if (!(flags & VMM_WRITABLE))
        attr |= ATTR_AP_RO;
    if (flags & VMM_USER) {
        attr |= ATTR_AP_USER;
        attr |= ATTR_PXN;
    } else {
        attr |= ATTR_UXN;
    }
    if (flags & VMM_COW)
        attr |= ATTR_AP_RO;
    return attr;
}

static int map_pte(page_directory_t *l0, uint64_t phys, uint64_t virt, uint32_t flags) {
    uint64_t *pte = walk_pt(l0, virt, 1);
    if (!pte) return -1;
    *pte = (phys & ADDR_MASK) | pte_flags(flags);
    __asm__ __volatile__("dsb sy\n\tisb");
    inv_page(virt);

    return 0;
}

static int map_1gb_block(page_directory_t *l1, uint64_t phys, uint64_t virt, uint32_t flags) {
    uint64_t l1_idx = L1_IDX(virt);
    if (l1->entries[l1_idx] & DESC_VALID)
        return -1;
    uint64_t attr = DESC_VALID | ATTR_AF;
    if (flags & VMM_CACHE_DISABLE)
        attr |= ATTR_ATTR_IDX(MT_DEVICE);
    else
        attr |= ATTR_ATTR_IDX(MT_NORMAL) | ATTR_SH(SH_INNER);
    l1->entries[l1_idx] = (phys & ~0x3FFFFFFFULL) | attr;
    return 0;
}

void vmm_init(void) {
    debug_print("vmm: init\n");

    initial_l0 = (page_directory_t *)pmm_alloc_page();
    if (!initial_l0) { debug_print("vmm: failed to alloc L0\n"); return; }
    memset(initial_l0, 0, sizeof(page_directory_t));
    debug_printf("vmm: L0 at %p\n", initial_l0);

    page_directory_t *l1 = (page_directory_t *)pmm_alloc_page();
    if (!l1) { debug_print("vmm: failed to alloc L1\n"); return; }
    memset(l1, 0, sizeof(page_directory_t));
    debug_printf("vmm: L1 at %p\n", l1);

    initial_l0->entries[0] = (uint64_t)(uintptr_t)l1 | DESC_VALID | DESC_TABLE | ATTR_AF;
    debug_printf("vmm: L0[0] = %lx\n", initial_l0->entries[0]);

    debug_print("vmm: mapping RAM...\n");
    /* Allocate L2 page table and wire it under L1[1] */
    page_directory_t *ram_l2 = (page_directory_t *)pmm_alloc_page();
    if (!ram_l2) { debug_print("vmm: failed to alloc L2\n"); return; }
    memset(ram_l2, 0, sizeof(page_directory_t));
    l1->entries[1] = (uint64_t)(uintptr_t)ram_l2 | DESC_VALID | DESC_TABLE | ATTR_AF;
    /* Map entire 64MB RAM as 2MB blocks (32 entries) under L2 */
    for (int i = 0; i < 32; i++) {
        page_directory_t *l3 = (page_directory_t *)pmm_alloc_page();
        if (!l3) { debug_print("vmm: failed to alloc L3\n"); return; }
        memset(l3, 0, sizeof(page_directory_t));
        uint64_t block_base = RAM_BASE + (uint64_t)i * 0x200000ULL;
        for (int j = 0; j < 512; j++) {
            uint64_t phys = block_base + (uint64_t)j * 0x1000ULL;
            l3->entries[j] = phys | DESC_PAGE | ATTR_AF
                           | ATTR_ATTR_IDX(MT_NORMAL) | ATTR_SH(SH_INNER);
        }
        ram_l2->entries[i] = (uint64_t)(uintptr_t)l3 | DESC_VALID | DESC_TABLE | ATTR_AF;
    }

    debug_print("vmm: mapping UART...\n");
    map_pte(initial_l0, UART_BASE, UART_BASE, VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);
    debug_print("vmm: mapping GICD...\n");
    map_pte(initial_l0, GICD_BASE, GICD_BASE, VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);
    debug_print("vmm: mapping GICC...\n");
    map_pte(initial_l0, GICC_BASE, GICC_BASE, VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);

    debug_print("vmm: setting MAIR...\n");
    uint64_t mair = (0xFFULL << (MT_NORMAL * 8)) | (0x00ULL << (MT_DEVICE * 8));
    __asm__ __volatile__("msr mair_el1, %0" : : "r"(mair));

    debug_print("vmm: setting TCR...\n");
    uint64_t tcr = TCR_T0SZ(48) | TCR_TG0_4KB | TCR_SH0_INNER | TCR_ORGN0_WBWA | TCR_IRGN0_WBWA
                 | TCR_IPS_40BIT;
    __asm__ __volatile__("msr tcr_el1, %0" : : "r"(tcr));

    debug_print("vmm: setting TTBR0...\n");
    uint64_t ttbr0 = (uint64_t)(uintptr_t)initial_l0;
    __asm__ __volatile__("msr ttbr0_el1, %0" : : "r"(ttbr0));

    __asm__ __volatile__("dsb ish\n\tisb");

    debug_print("vmm: enabling MMU...\n");
    uint64_t sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_MMU | SCTLR_CACHE | SCTLR_I | SCTLR_SA0 | SCTLR_SA;
    __asm__ __volatile__("msr sctlr_el1, %0" : : "r"(sctlr));
    __asm__ __volatile__("isb");
    __asm__ __volatile__("dsb ish\n\ttlbi vmalle1is\n\tdsb ish\n\tisb");
    debug_print("vmm: MMU enabled!\n");

    kernel_l1 = initial_l0;
    current_l1 = initial_l0;
    mmu_on = 1;

    debug_print("vmm: init done\n");

    /* Quick sanity: verify vmm_temp_map works */
    void *tmphys = pmm_alloc_page();
    void *tmpvirt = vmm_temp_map((uint64_t)(uintptr_t)tmphys);
    debug_print("vmm: test vmm_temp_map phys=");
    debug_print_hex32((uint32_t)(uintptr_t)tmphys);
    debug_print(" virt=");
    debug_print_hex32((uint32_t)(uintptr_t)tmpvirt);
    debug_print("\n");
    if (tmpvirt) {
        /* Test writing to TEMP_VADDR */
        volatile uint64_t *test_ptr = (volatile uint64_t *)tmpvirt;
        *test_ptr = 0xDEADBEEFCAFEBABEULL;
        __asm__ __volatile__("dsb sy\n\tisb");
        uint64_t readback = *test_ptr;
        debug_print("vmm: wrote=");
        debug_print_hex32(0xCAFEBABE);
        debug_print_hex32(0xDEADBEEF);
        debug_print(" read=");
        debug_print_hex32((uint32_t)(readback >> 32));
        debug_print_hex32((uint32_t)readback);
        debug_print("\n");
        vmm_temp_unmap();
        debug_print("vmm: vmm_temp_map test OK\n");
    } else {
        debug_print("vmm: vmm_temp_map test FAILED\n");
    }
}

void vmm_init_heap(void) {
}

page_directory_t *vmm_create_directory(void) {
    if (!kernel_l1) return NULL;

    page_directory_t *l0 = (page_directory_t *)pmm_alloc_page();
    if (!l0) return NULL;
    memset(l0, 0, sizeof(page_directory_t));

    for (int i = 0; i < 512; i++) {
        if (initial_l0->entries[i] & DESC_VALID) {
            uint64_t entry = initial_l0->entries[i];
            if (entry & DESC_TABLE) {
                page_directory_t *child = (page_directory_t *)(uintptr_t)(entry & ADDR_MASK);
                l0->entries[i] = entry;
                (void)child;
            } else {
                l0->entries[i] = entry;
            }
        }
    }

    return l0;
}

void vmm_switch_directory(page_directory_t *dir) {
    if (!dir) return;

    if (!mmu_on) return;

    uint64_t ttbr0 = (uint64_t)(uintptr_t)dir;
    __asm__ __volatile__("msr ttbr0_el1, %0\n\t"
                         "dsb ish\n\t"
                         "tlbi vmalle1is\n\t"
                         "dsb ish\n\t"
                         "isb" : : "r"(ttbr0));
    current_l1 = dir;
}

void vmm_free_directory(page_directory_t *dir) {
    (void)dir;
}

/* ---- PCI ECAM mapping ---- */

/* Virtual address for PCI ECAM config space (2MB block to cover bus 0). */
#define ECAM_VADDR 0xFFE00000ULL

/* Map the first 2MB of PCI ECAM config space at ECAM_VADDR.
 * 2MB covers one full PCI bus (32 devices × 8 functions × 4096 bytes).
 * Returns 0 on success, -1 on failure. */
int vmm_map_pci_ecam(void) {
    if (!pci_ecam_base || !pci_ecam_size) {
        debug_print("vmm: no PCI ECAM base, skipping mapping\n");
        return -1;
    }
    if (pci_ecam_base & 0x1FFFFFULL) {
        debug_print("vmm: PCI ECAM base not 2MB-aligned\n");
        return -1;
    }
    debug_printf("vmm: mapping PCI ECAM at phys 0x%lx -> virt 0x%lx\n",
                 pci_ecam_base, ECAM_VADDR);

    page_directory_t *l0 = initial_l0;
    if (!l0) l0 = kernel_l1;
    if (!l0) return -1;

    /* Find/create L1 for the ECAM virtual address range. */
    page_directory_t *l1 = get_l1(l0, ECAM_VADDR, 1);
    if (!l1) {
        debug_print("vmm: failed to get L1 for ECAM\n");
        return -1;
    }

    /* Find/create L2 table under L1[L1_IDX(ECAM_VADDR)].
     * L1 entry at this index must be a table pointer (not block). */
    uint64_t l1_idx = L1_IDX(ECAM_VADDR);
    if (!(l1->entries[l1_idx] & DESC_VALID)) {
        page_directory_t *l2 = (page_directory_t *)pmm_alloc_page();
        if (!l2) {
            debug_print("vmm: failed to alloc L2 for ECAM\n");
            return -1;
        }
        memset(l2, 0, sizeof(page_directory_t));
        l1->entries[l1_idx] = (uint64_t)(uintptr_t)l2
                            | DESC_VALID | DESC_TABLE | ATTR_AF;
    }
    page_directory_t *l2 = (page_directory_t *)(uintptr_t)
                           (l1->entries[l1_idx] & ADDR_MASK);

    uint64_t l2_idx = L2_IDX(ECAM_VADDR);
    if (l2->entries[l2_idx] & DESC_VALID) {
        debug_print("vmm: ECAM L2 entry already in use\n");
        return -1;
    }

    /* Create a 2MB block mapping at L2 for ECAM.
     * L2 block entry: bit[1]=0, bit[0]=1 → 0b01.
     * Using DEVICE memory attributes (nGnRE). */
    uint64_t entry = (pci_ecam_base & 0xFFFFFFFFFFE00000ULL)
                   | DESC_VALID
                   | ATTR_AF
                   | ATTR_ATTR_IDX(MT_DEVICE);
    l2->entries[l2_idx] = entry;
    __asm__ __volatile__("dsb sy\n\tisb");
    debug_print("vmm: PCI ECAM mapped\n");
    return 0;
}

page_directory_t *vmm_get_current_directory(void) {
    return current_l1;
}

page_directory_t *vmm_get_kernel_directory(void) {
    return kernel_l1;
}

int vmm_map_page(page_directory_t *dir, uint64_t phys, uint64_t virt, uint32_t flags) {
    return map_pte(dir, phys, virt, flags);
}

void vmm_unmap_page(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_pt(dir, virt, 0);
    if (pte) {
        *pte = 0;
        inv_page(virt);
    }
}

uint64_t vmm_get_physical(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_pt(dir, virt, 0);
    if (pte && (*pte & DESC_VALID))
        return (*pte & ADDR_MASK) | (virt & 0xFFF);
    return 0;
}

int vmm_get_page_flags(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_pt(dir, virt, 0);
    if (!pte) return 0;
    int flags = 0;
    if (*pte & DESC_VALID) flags |= VMM_PRESENT;
    if (!(*pte & ATTR_AP_RO)) flags |= VMM_WRITABLE;
    if (*pte & ATTR_AP_USER) flags |= VMM_USER;
    return flags;
}

int vmm_is_page_present(page_directory_t *dir, uint64_t virt) {
    uint64_t *pte = walk_pt(dir, virt, 0);
    return (pte && (*pte & DESC_VALID)) ? 1 : 0;
}

static page_fault_handler_t user_fault_handler;

void vmm_register_fault_handler(page_fault_handler_t handler) {
    user_fault_handler = handler;
}

static int handle_cow(page_directory_t *l0, uint64_t fault_addr) {
    uint64_t *pte = walk_pt(l0, fault_addr, 0);
    if (!pte) return 0;
    if (!(*pte & DESC_VALID)) return 0;
    if (!(*pte & ATTR_AP_RO)) return 0;
    if (!(*pte & DESC_PAGE)) return 0;

    uint64_t old_phys = *pte & ADDR_MASK;
    uint64_t flags = *pte & ~(ADDR_MASK | ATTR_AP_RO);
    flags |= ATTR_AF;
    if (*pte & ATTR_AP_USER)
        flags |= ATTR_AP_USER;
    else
        flags |= ATTR_UXN;

    void *new_phys = pmm_alloc_page();
    if (!new_phys) return 0;
    memcpy((void *)(uintptr_t)new_phys, (void *)(uintptr_t)old_phys, PAGE_SIZE);

    *pte = ((uint64_t)(uintptr_t)new_phys & ADDR_MASK) | flags;
    inv_page(fault_addr);
    return 1;
}

void vmm_fork_cow_pages(page_directory_t *parent_dir, page_directory_t *child_dir) {
    if (!parent_dir || !child_dir) return;

    for (int l0i = 0; l0i < 512; l0i++) {
        if (!(parent_dir->entries[l0i] & DESC_VALID)) continue;
        if (!(parent_dir->entries[l0i] & DESC_TABLE)) continue;

        page_directory_t *parent_l1 = (page_directory_t *)(uintptr_t)(parent_dir->entries[l0i] & ADDR_MASK);
        page_directory_t *child_l1;
        if (!(child_dir->entries[l0i] & DESC_VALID)) continue;
        if (!(child_dir->entries[l0i] & DESC_TABLE)) continue;
        child_l1 = (page_directory_t *)(uintptr_t)(child_dir->entries[l0i] & ADDR_MASK);

        for (int l1i = 0; l1i < 512; l1i++) {
            if (!(parent_l1->entries[l1i] & DESC_VALID)) continue;
            if (!(parent_l1->entries[l1i] & DESC_TABLE)) continue;

            page_directory_t *parent_l2 = (page_directory_t *)(uintptr_t)(parent_l1->entries[l1i] & ADDR_MASK);
            page_directory_t *child_l2;
            if (!(child_l1->entries[l1i] & DESC_VALID)) continue;
            if (!(child_l1->entries[l1i] & DESC_TABLE)) continue;
            child_l2 = (page_directory_t *)(uintptr_t)(child_l1->entries[l1i] & ADDR_MASK);

            for (int l2i = 0; l2i < 512; l2i++) {
                if (!(parent_l2->entries[l2i] & DESC_VALID)) continue;
                if (!(parent_l2->entries[l2i] & DESC_TABLE)) continue;

                page_directory_t *parent_l3 = (page_directory_t *)(uintptr_t)(parent_l2->entries[l2i] & ADDR_MASK);
                page_directory_t *child_l3;
                if (!(child_l2->entries[l2i] & DESC_VALID)) continue;
                if (!(child_l2->entries[l2i] & DESC_TABLE)) continue;
                child_l3 = (page_directory_t *)(uintptr_t)(child_l2->entries[l2i] & ADDR_MASK);

                for (int l3i = 0; l3i < 512; l3i++) {
                    uint64_t pte = parent_l3->entries[l3i];
                    if (!(pte & DESC_VALID)) continue;
                    if (!(pte & DESC_PAGE)) continue;
                    if (!(pte & ATTR_AP_USER)) continue;
                    if (pte & ATTR_AP_RO) continue;

                    parent_l3->entries[l3i] = pte | ATTR_AP_RO;
                    child_l3->entries[l3i] = parent_l3->entries[l3i];
                }
            }
        }
    }
}

void vmm_clear_user_pages(page_directory_t *dir) {
    if (!dir) return;

    for (int l0i = 0; l0i < 512; l0i++) {
        if (!(dir->entries[l0i] & DESC_VALID)) continue;
        if (!(dir->entries[l0i] & DESC_TABLE)) continue;

        page_directory_t *l1 = (page_directory_t *)(uintptr_t)(dir->entries[l0i] & ADDR_MASK);
        for (int l1i = 0; l1i < 512; l1i++) {
            if (!(l1->entries[l1i] & DESC_VALID)) continue;
            if ((l1->entries[l1i] & 3) == 1) {
                if (l1->entries[l1i] & ATTR_AP_USER) {
                    l1->entries[l1i] = 0;
                }
                continue;
            }
            if (!(l1->entries[l1i] & DESC_TABLE)) continue;

            page_directory_t *l2 = (page_directory_t *)(uintptr_t)(l1->entries[l1i] & ADDR_MASK);
            for (int l2i = 0; l2i < 512; l2i++) {
                if (!(l2->entries[l2i] & DESC_VALID)) continue;
                if ((l2->entries[l2i] & 3) == 1) {
                    if (l2->entries[l2i] & ATTR_AP_USER) {
                        l2->entries[l2i] = 0;
                    }
                    continue;
                }
                if (!(l2->entries[l2i] & DESC_TABLE)) continue;

                page_directory_t *l3 = (page_directory_t *)(uintptr_t)(l2->entries[l2i] & ADDR_MASK);
                for (int l3i = 0; l3i < 512; l3i++) {
                    if (l3->entries[l3i] & ATTR_AP_USER)
                        l3->entries[l3i] = 0;
                }
            }
        }
    }
    __asm__ __volatile__("dsb ish\n\ttlbi vmalle1is\n\tdsb ish\n\tisb");
}

/* ---- Heap ---- */
typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    struct heap_block *next;
} heap_block_t;

#define HEAP_MAGIC_FREE 0x48454150
#define HEAP_MAGIC_USED 0x44454144
#define HEAP_SIZE_MASK  0x7FFFFFFF

extern char __heap_start[];
extern char __heap_end[];

static heap_block_t *heap_base;
static heap_block_t *heap_free_list;
static int heap_initialized;

static void heap_init(void) {
    heap_base = (heap_block_t *)__heap_start;
    memset(heap_base, 0, sizeof(heap_block_t));
    heap_base->magic = HEAP_MAGIC_FREE;
    heap_base->size = (uint64_t)(__heap_end - __heap_start) - sizeof(heap_block_t);
    heap_base->next = NULL;
    heap_free_list = heap_base;
    heap_initialized = 1;
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;

    if (!heap_initialized) heap_init();

    size = (size + 3) & ~3;
    if (size < 16) size = 16;

    heap_block_t *prev = NULL;
    heap_block_t *block = heap_free_list;
    while (block) {
        if (block->magic != HEAP_MAGIC_FREE) {
            heap_free_list = block->next;
            block = heap_free_list;
            prev = NULL;
            continue;
        }
        uint32_t block_size = block->size & HEAP_SIZE_MASK;
        if (block_size >= size) {
            if (block_size >= size + sizeof(heap_block_t) + 16) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + sizeof(heap_block_t) + size);
                new_block->magic = HEAP_MAGIC_FREE;
                new_block->size = block_size - size - sizeof(heap_block_t);
                new_block->next = block->next;
                block->size = size | HEAP_MAGIC_USED;
                block->next = new_block;
            } else {
                block->magic = HEAP_MAGIC_USED;
            }
            if (prev)
                prev->next = block->next;
            else
                heap_free_list = block->next;
            memset((uint8_t *)block + sizeof(heap_block_t), 0, size);
            return (void *)((uint8_t *)block + sizeof(heap_block_t));
        }
        prev = block;
        block = block->next;
    }
    return NULL;
}

void *kcalloc(uint32_t count, uint32_t size) {
    uint32_t total = count * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void kfree(void *addr) {
    if (!addr) return;
    heap_block_t *block = (heap_block_t *)((uint8_t *)addr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC_USED) return;
    block->magic = HEAP_MAGIC_FREE;
    block->next = heap_free_list;
    heap_free_list = block;
}

void *vmm_temp_map(uint64_t phys) {
    if (vmm_map_page(current_l1 ? current_l1 : kernel_l1, phys, TEMP_VADDR, VMM_PRESENT | VMM_WRITABLE) < 0) {
        return NULL;
    }
    return (void *)TEMP_VADDR;
}

void vmm_temp_unmap(void) {
    vmm_unmap_page(current_l1 ? current_l1 : kernel_l1, TEMP_VADDR);
}

/* ---- User copy ---- */
int copy_from_user(void *dst, const void *user_src, uint32_t size) {
    if (size == 0) return 0;
    page_directory_t *dir = vmm_get_current_directory();
    for (uint32_t offset = 0; offset < size; ) {
        uint64_t vaddr = (uint64_t)(uintptr_t)user_src + offset;
        uint64_t *pte = walk_pt(dir, vaddr, 0);
        if (!pte || !(*pte & DESC_VALID) || !(*pte & ATTR_AP_USER))
            return -1;
        uint32_t chunk = PAGE_SIZE - (vaddr & 0xFFF);
        if (chunk > size - offset) chunk = size - offset;
        memcpy((uint8_t *)dst + offset, (uint8_t *)(uintptr_t)vaddr, chunk);
        offset += chunk;
    }
    return 0;
}

int copy_to_user(void *user_dst, const void *src, uint32_t size) {
    if (size == 0) return 0;
    page_directory_t *dir = vmm_get_current_directory();
    for (uint32_t offset = 0; offset < size; ) {
        uint64_t vaddr = (uint64_t)(uintptr_t)user_dst + offset;
        uint64_t *pte = walk_pt(dir, vaddr, 0);
        if (!pte || !(*pte & DESC_VALID) || !(*pte & ATTR_AP_USER))
            return -1;
        uint32_t chunk = PAGE_SIZE - (vaddr & 0xFFF);
        if (chunk > size - offset) chunk = size - offset;
        memcpy((uint8_t *)(uintptr_t)vaddr, (const uint8_t *)src + offset, chunk);
        offset += chunk;
    }
    return 0;
}

int strncpy_from_user(char *dst, const char *user_src, uint32_t max_len) {
    page_directory_t *dir = vmm_get_current_directory();
    for (uint32_t i = 0; i < max_len; i++) {
        uint64_t vaddr = (uint64_t)(uintptr_t)user_src + i;
        uint64_t *pte = walk_pt(dir, vaddr, 0);
        if (!pte || !(*pte & DESC_VALID) || !(*pte & ATTR_AP_USER))
            return -1;
        char c = *(volatile char *)(uintptr_t)vaddr;
        dst[i] = c;
        if (c == '\0') return (int)i;
    }
    dst[max_len - 1] = '\0';
    return (int)max_len - 1;
}

int vmm_handle_page_fault(registers_t *r, uint64_t fault_addr, uint32_t esr) {
    uint32_t ec = (esr >> 26) & 0x3F;
    int write = (esr >> 6) & 1;

    if (ec != 0x24 && ec != 0x25)
        return 0;

    page_directory_t *dir = vmm_get_current_directory();
    uint64_t *pte = walk_pt(dir, fault_addr, 0);

    if (pte && (*pte & DESC_VALID)) {
        if ((*pte & ATTR_AP_RO) && (write || ec == 0x25)) {
            if (handle_cow(dir, fault_addr))
                return 1;
        }
    }

    if (ec == 0x24) {
        if (user_fault_handler) {
            user_fault_handler(r, fault_addr, esr);
            return 1;
        }
    }

    return 0;
}
