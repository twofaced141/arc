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


#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "memory.h"
#include "registers.h"

#define VMM_PRESENT      (1 << 0)
#define VMM_WRITABLE     (1 << 1)
#define VMM_USER         (1 << 2)
/* Shared code uses VMM_NX for no-execute; arm64 vmm_map_page maps it
 * to PXN/UXN attributes. */
#define VMM_NX           (1ULL << 63)
#define VMM_WRITE_THROUGH (1 << 3)
#define VMM_CACHE_DISABLE (1 << 4)
#define VMM_ACCESSED     (1 << 5)
#define VMM_DIRTY        (1 << 6)

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITE    (1 << 1)
#define VMM_FLAG_USER     (1 << 2)
#define VMM_FLAG_RESERVED (1 << 3)
#define VMM_FLAG_FETCH    (1 << 4)
#define VMM_COW           (1 << 9)

typedef struct page_directory {
    uint64_t entries[512];
} __attribute__((aligned(4096))) page_directory_t;

typedef void (*page_fault_handler_t)(registers_t *r, uint64_t fault_addr,
                                     uint32_t error_code);

void vmm_init(void);
void vmm_init_heap(void);

page_directory_t *vmm_create_directory(void);
void vmm_switch_directory(page_directory_t *dir);
void vmm_free_directory(page_directory_t *dir);
page_directory_t *vmm_get_current_directory(void);
page_directory_t *vmm_get_kernel_directory(void);

int vmm_map_page(page_directory_t *dir, uint64_t phys, uint64_t virt, uint64_t flags);
void vmm_unmap_page(page_directory_t *dir, uint64_t virt);
uint64_t vmm_get_physical(page_directory_t *dir, uint64_t virt);
int vmm_get_page_flags(page_directory_t *dir, uint64_t virt);
/* Break copy-on-write sharing on a user page (mprotect PROT_WRITE).
 * Returns 1 if the page was privatized, 0 if it was not COW. */
int vmm_cow_break(page_directory_t *dir, uint64_t virt);
int vmm_is_page_present(page_directory_t *dir, uint64_t virt);

void vmm_register_fault_handler(page_fault_handler_t handler);
void vmm_fork_cow_pages(page_directory_t *parent_dir, page_directory_t *child_dir);
void vmm_clear_user_pages(page_directory_t *dir);

void *kmalloc(uint32_t size);
void *kcalloc(uint32_t count, uint32_t size);
void  kfree(void *addr);

#define TEMP_VADDR 0x00007FFFFFFFF000ULL
void *vmm_temp_map(uint64_t phys);
void  vmm_temp_unmap(void);

int copy_from_user(void *dst, const void *user_src, uint32_t size);
int copy_to_user(void *user_dst, const void *src, uint32_t size);
int user_range_ok(const void *uaddr, uint32_t size, int write);
int strncpy_from_user(char *dst, const char *user_src, uint32_t max_len);

int vmm_handle_page_fault(registers_t *r, uint64_t fault_addr, uint32_t esr);

/* Map PCI ECAM config space (from FDT-discovered base) at ECAM_VADDR. */
int vmm_map_pci_ecam(void);

#endif