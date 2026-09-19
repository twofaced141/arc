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
#include "personality.h"
#include "thread.h"
#include "cpu.h"
#include <string.h>

static page_directory_t *kernel_directory;
static page_directory_t *current_directory;

static inline void invlpg(uintptr_t addr) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void switch_cr3(uintptr_t dir) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(dir) : "memory");
}

static inline uintptr_t read_cr0(void) {
    uintptr_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void write_cr0(uintptr_t cr0) {
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0));
}

static inline uintptr_t read_cr2(void) {
    uintptr_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static page_table_t *get_table(page_directory_t *dir, uint32_t pd_index, int create) {
    if (dir->entries[pd_index] & VMM_PRESENT)
        return (page_table_t *)(dir->entries[pd_index] & ~0xFFF);

    if (!create)
        return NULL;

    page_table_t *table = (page_table_t *)pmm_alloc_page();
    if (!table)
        return NULL;

    for (int i = 0; i < 1024; i++)
        table->entries[i] = 0;

    dir->entries[pd_index] = (uint32_t)table | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return table;
}

page_directory_t *vmm_create_directory(void) {
    page_directory_t *dir = (page_directory_t *)pmm_alloc_page();
    if (!dir)
        return NULL;

    for (int i = 0; i < 1024; i++)
        dir->entries[i] = 0;

    for (int i = 0; i < 1024; i++) {
        if (kernel_directory->entries[i] & VMM_PRESENT)
            dir->entries[i] = kernel_directory->entries[i];
    }

    return dir;
}

void vmm_switch_directory(page_directory_t *dir) {
    current_directory = dir;
    switch_cr3((uint32_t)dir);
}

page_directory_t *vmm_get_current_directory(void) {
    return current_directory;
}

page_directory_t *vmm_get_kernel_directory(void) {
    return kernel_directory;
}

void vmm_fork_cow_pages(page_directory_t *parent_dir, page_directory_t *child_dir) {
    if (!parent_dir || !child_dir) return;

    for (int i = 0; i < 1024; i++) {
        if (!(parent_dir->entries[i] & VMM_PRESENT))
            continue;

        uint32_t virt_base = (uint32_t)i * 1024 * PAGE_SIZE;
        if (virt_base >= KERNEL_BASE)
            continue;

        page_table_t *parent_table = (page_table_t *)(parent_dir->entries[i] & ~0xFFF);
        page_table_t *child_table = (page_table_t *)(child_dir->entries[i] & ~0xFFF);

        if (!(child_dir->entries[i] & VMM_PRESENT)) {
            page_table_t *new_table = (page_table_t *)pmm_alloc_page();
            if (!new_table) continue;
            for (int j = 0; j < 1024; j++)
                new_table->entries[j] = 0;
            child_dir->entries[i] = (uint32_t)new_table | (parent_dir->entries[i] & 0xFFF);
            child_table = new_table;
        }

        for (int j = 0; j < 1024; j++) {
            if (!(parent_table->entries[j] & VMM_PRESENT))
                continue;

            /* Pages already COW (marked by an earlier fork) keep their COW
             * PTE — the new child just shares the same read-only page.
             * Skipping them would leave the child's copy of the PTE zeroed
             * (its text/data unmapped entirely). */
            uint32_t flags = parent_table->entries[j] & 0xFFF;
            flags &= ~VMM_WRITABLE;
            flags |= VMM_COW;

            parent_table->entries[j] = (parent_table->entries[j] & ~0xFFF) | flags;
            child_table->entries[j] = parent_table->entries[j];
        }
    }

    /* Parent pages just became read-only: drop this CPU's stale writable
     * TLB entries, then synchronously shoot down every other online CPU
     * (same COW protocol as amd64 — previously there was no flush here
     * at all, so a sibling thread on another CPU could write through a
     * stale entry into a now-shared page). */
    vmm_tlb_reload_current();
    tlb_flush_others_sync();
}

void vmm_free_directory(page_directory_t *dir) {
    if (dir == kernel_directory || dir == current_directory)
        return;

    for (int i = 0; i < 1024; i++) {
        if (!(dir->entries[i] & VMM_PRESENT))
            continue;

        uint32_t virt_base = (uint32_t)i * 1024 * PAGE_SIZE;
        if (virt_base >= KERNEL_BASE)
            continue;

        if (dir->entries[i] == kernel_directory->entries[i])
            continue;

        page_table_t *table = (page_table_t *)(dir->entries[i] & ~0xFFF);

        for (int j = 0; j < 1024; j++) {
            uint32_t pte = table->entries[j];
            if ((pte & VMM_PRESENT) && !(pte & VMM_COW))
                /* COW pages are shared with another process (e.g. the
                 * parent's text); freeing them here would free memory
                 * the parent still uses. */
                pmm_free_page((void *)(pte & ~0xFFF));
        }

        pmm_free_page(table);
    }

    pmm_free_page(dir);
}

int vmm_map_page(page_directory_t *dir, uint64_t phys, uint64_t virt, uint64_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    page_table_t *table = get_table(dir, pd_index, 1);
    if (!table)
        return -1;

    table->entries[pt_index] = (phys & ~0xFFF) | flags;
    invlpg(virt);
    return 0;
}

void vmm_unmap_page(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    table->entries[pt_index] = 0;
    invlpg(virt);
}

uint32_t vmm_get_physical(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);

    if (!(table->entries[pt_index] & VMM_PRESENT))
        return 0;

    return (table->entries[pt_index] & ~0xFFF) | (virt & 0xFFF);
}

int vmm_get_page_flags(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    return table->entries[pt_index] & 0xFFF;
}

int vmm_is_page_present(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    return (table->entries[pt_index] & VMM_PRESENT) != 0;
}

/* Copy-on-write core.  Public so mprotect(PROT_WRITE) can break
 * sharing explicitly instead of silently keeping parent and child on
 * one physical page.  Returns 1 if the page was broken, 0 otherwise. */
int vmm_cow_break(page_directory_t *dir, uintptr_t fault_addr) {
    uint32_t pd_index = (uint32_t)fault_addr >> 22;
    uint32_t pt_index = ((uint32_t)fault_addr >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    if (!(table->entries[pt_index] & VMM_PRESENT))
        return 0;
    if (!(table->entries[pt_index] & VMM_COW))
        return 0;

    uint32_t old_phys = table->entries[pt_index] & ~0xFFF;
    void *new_page = pmm_alloc_page();
    if (!new_page)
        return 0;
    uint32_t new_phys = (uint32_t)(uintptr_t)new_page;

    void *old_va = vmm_temp_map(old_phys);
    if (!old_va) {
        pmm_free_page(new_page);
        return 0;
    }

    uint32_t flags = table->entries[pt_index] & 0xFFF;
    flags &= ~(VMM_WRITABLE | VMM_COW);
    flags |= VMM_WRITABLE;
    table->entries[pt_index] = (new_phys & ~0xFFF) | flags;
    invlpg(fault_addr);

    memcpy((void *)(uintptr_t)((uint32_t)fault_addr & ~0xFFF), old_va, PAGE_SIZE);
    vmm_temp_unmap();
    return 1;
}

/* Copy-on-write: the faulting page is COW (read-only, VMM_COW set).
 * Allocate a private page, copy the shared contents through the temp
 * slot, and remap the faulting address writable. */
static int handle_cow_fault(uint32_t fault_addr) {
    page_directory_t *dir = vmm_get_current_directory();
    return vmm_cow_break(dir, fault_addr);
}

void page_fault_handler(registers_t *r) {
    uintptr_t fault_addr = read_cr2();
    uint32_t error_code = (uint32_t)r->err_code;

    if (error_code & 0x4) {
        if ((error_code & 0x1) && handle_cow_fault(fault_addr))
            return;
        debug_printf("Page fault at 0x%x (err=0x%x, eip=0x%x) "
                     "in user mode — terminating process\r\n",
                     fault_addr, error_code, r->eip);
        if (proc_current && proc_exit) {
            struct proc *p = proc_current();
            if (p) {
                proc_exit(139, r);
                thread_exit(139);
                return;
            }
        }
    }

    debug_print("PAGE FAULT at 0x");
    debug_print_hex32(fault_addr);
    debug_print(" err=0x");
    debug_print_hex32(error_code);
    debug_print(" eip=0x");
    debug_print_hex32(r->eip);
    debug_print("\r\n");

    panic("Page Fault", r);
}

/* Heap now unified in mk/vm/heap.c — HAL provides heap_init/kmalloc/kfree */
void *vmm_temp_map(uint32_t phys) {
    if (vmm_map_page(kernel_directory, phys, TEMP_VADDR, VMM_PRESENT | VMM_WRITABLE) < 0)
        return NULL;
    return (void *)TEMP_VADDR;
}

void vmm_temp_unmap(void) {
    vmm_unmap_page(kernel_directory, TEMP_VADDR);
    invlpg(TEMP_VADDR);
}

/* Validate a user range without touching it (see amd64 vmm.c). */
int user_range_ok(const void *uaddr, size_t size, int write) {
    if (size == 0) return 1;
    if (!uaddr) return 0;
    size_t addr = (size_t)uaddr;
    if (addr > 0xC0000000u || size > 0xC0000000u - addr) return 0;
    page_directory_t *dir = vmm_get_current_directory();
    size_t first = addr & ~0xFFFu, last = (addr + size - 1) & ~0xFFFu;
    for (size_t page = first; ; page += PAGE_SIZE) {
        int flags = vmm_get_page_flags(dir, page);
        if (!(flags & VMM_PRESENT)) return 0;
        if (!(flags & VMM_USER)) return 0;
        if (write && !(flags & (VMM_WRITABLE | VMM_COW))) return 0;
        if (page == last) break;
    }
    return 1;
}

int copy_from_user(void *dst, const void *user_src, size_t size) {
    if (size == 0) return 0;
    size_t addr = (size_t)user_src;
    if (addr + size < addr) return -1;
    if (addr + size > 0xC0000000) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    for (size_t offset = 0; offset < size; ) {
        size_t vaddr = addr + offset;
        int flags = vmm_get_page_flags(dir, vaddr & ~0xFFFu);
        if (!(flags & VMM_PRESENT)) return -1;
        if (!(flags & VMM_USER)) return -1;
        size_t chunk = PAGE_SIZE - (vaddr & 0xFFFu);
        if (chunk > size - offset) chunk = size - offset;
        for (size_t i = 0; i < chunk; i++)
            ((uint8_t *)dst)[offset + i] = ((const uint8_t *)user_src)[offset + i];
        offset += chunk;
    }
    return 0;
}

int copy_to_user(void *user_dst, const void *src, size_t size) {
    if (size == 0) return 0;
    size_t addr = (size_t)user_dst;
    if (addr + size < addr) return -1;
    if (addr + size > 0xC0000000) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    for (size_t offset = 0; offset < size; ) {
        size_t vaddr = addr + offset;
        int flags = vmm_get_page_flags(dir, vaddr & ~0xFFFu);
        if (!(flags & VMM_PRESENT)) return -1;
        if (!(flags & VMM_USER)) return -1;
        if (!(flags & VMM_WRITABLE) && !(flags & VMM_COW)) return -1;
        size_t chunk = PAGE_SIZE - (vaddr & 0xFFFu);
        if (chunk > size - offset) chunk = size - offset;
        for (size_t i = 0; i < chunk; i++)
            ((uint8_t *)user_dst)[offset + i] = ((const uint8_t *)src)[offset + i];
        offset += chunk;
    }
    return 0;
}

int strncpy_from_user(char *dst, const char *user_src, size_t max_len) {
    if (max_len == 0) return -1;
    size_t addr = (size_t)user_src;
    if (addr >= 0xC0000000) return -1;
    size_t avail = 0xC0000000 - addr;
    if (max_len > avail) max_len = avail;
    if (max_len == 0) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    size_t end_page = (addr + max_len + PAGE_SIZE - 1) & ~0xFFF;
    for (size_t page = addr & ~0xFFF; page < end_page; page += PAGE_SIZE) {
        if (!vmm_is_page_present(dir, page)) return -1;
        if (!(vmm_get_page_flags(dir, page) & VMM_USER)) return -1;
    }

    for (size_t i = 0; i < max_len; i++) {
        char c = ((const char *)user_src)[i];
        dst[i] = c;
        if (c == '\0') {
            if (i + 1 > (size_t)0x7FFFFFFF)
                return -1;
            return (int)(i + 1);
        }
    }
    return -1;
}

/* Full local TLB flush (used by the IPI_TLB handler on remote CPUs). */
void vmm_tlb_reload_current(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

void vmm_init(void) {
    uint32_t esp_val;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(esp_val));
    debug_print("vmm_init esp=0x");
    debug_print_hex32(esp_val);
    debug_print("\r\n");

    kernel_directory = (page_directory_t *)pmm_alloc_page();
    if (!kernel_directory)
        return;

    for (int i = 0; i < 1024; i++)
        kernel_directory->entries[i] = 0;

    /* Identity map first 64MB — page tables must be accessible by phys addr */
    for (uint32_t virt = 0; virt < 0x04000000; virt += PAGE_SIZE) {
        (void)vmm_map_page(kernel_directory, virt, virt, VMM_PRESENT | VMM_WRITABLE);
    }

    /* Kernel higher half: physical 1MB+ -> virtual 0xC0000000+ */
    for (uint32_t offset = 0; offset < 0x400000; offset += PAGE_SIZE) {
        (void)vmm_map_page(kernel_directory, KERNEL_PHYS + offset,
                      KERNEL_BASE + offset,
                      VMM_PRESENT | VMM_WRITABLE);
    }

    /* Pre-create TEMP_VADDR PDE so vmm_create_directory copies it for all processes */
    {
        uint32_t pd_idx = TEMP_VADDR >> 22;
        if (!(kernel_directory->entries[pd_idx] & VMM_PRESENT)) {
            page_table_t *t = (page_table_t *)pmm_alloc_page();
            for (int i = 0; i < 1024; i++) t->entries[i] = 0;
            kernel_directory->entries[pd_idx] = (uint32_t)t | VMM_PRESENT | VMM_WRITABLE;
        }
    }

    current_directory = kernel_directory;
    switch_cr3((uint32_t)kernel_directory);

    uint32_t cr0 = read_cr0();
    cr0 |= (1 << 31);
    write_cr0(cr0);

    register_interrupt_handler(14, page_fault_handler);

    debug_print("vmm: paging enabled\r\n");
    debug_print("vmm: kernel mapped at 0xC0000000 (higher half)\r\n");
    debug_print("vmm: identity map 0-64MB\r\n");
}
