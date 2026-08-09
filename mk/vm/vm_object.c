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
#include "pmm.h"
#include "string.h"
#include "debug.h"
#include "vmm.h"

#define PAGE_SIZE 4096

/* ================================================================
 * vm_object lifecycle
 * ================================================================ */

void vm_object_init(void) {
    log_print(LOG_LEVEL_DEBUG, "vm_object: init done\r\n");
}

/* ---- Self-test: shared memory page cache ---- */
void vm_object_test_shared(void) {
    vm_object_t *obj = vm_object_create_shared(PAGE_SIZE * 2);
    if (!obj)
        return;

    vm_page_t p1, p2, p3;
    uint64_t phys_addr1, phys_addr2, phys_addr3;

    if (obj->fault(obj, 0, &p1, 1) < 0)
        goto cleanup;
    phys_addr1 = p1.phys_addr;

    if (obj->fault(obj, 0, &p2, 1) < 0)
        goto cleanup;
    phys_addr2 = p2.phys_addr;

    if (phys_addr2 != phys_addr1)
        goto cleanup;

    if (obj->fault(obj, PAGE_SIZE, &p3, 1) < 0)
        goto cleanup;
    phys_addr3 = p3.phys_addr;

    if (phys_addr3 == phys_addr1)
        goto cleanup;

    void *va = vmm_temp_map(phys_addr1);
    if (!va)
        goto cleanup;
    volatile uint32_t *ptr = (volatile uint32_t *)va;
    *ptr = 0xDEADBEEF;
    vmm_temp_unmap();

    vm_page_t p4;
    if (obj->fault(obj, 0, &p4, 0) < 0)
        goto cleanup;
    if (p4.phys_addr != phys_addr1)
        goto cleanup;

    va = vmm_temp_map(phys_addr1);
    if (!va)
        goto cleanup;
    uint32_t val = *(volatile uint32_t *)va;
    vmm_temp_unmap();

    if (val != 0xDEADBEEF)
        goto cleanup;

cleanup:
    vm_object_release(obj);
}

static vm_object_t *vm_object_alloc(int type, uint64_t size) {
    if (size == 0)
        return NULL;

    vm_object_t *obj = (vm_object_t *)kmalloc(sizeof(vm_object_t));
    if (!obj)
        return NULL;

    memset(obj, 0, sizeof(vm_object_t));
    obj->ref_count = 1;
    obj->type = type;
    obj->size = size;
    obj->lock = SPINLOCK_INIT;
    return obj;
}

vm_object_t *vm_object_create_anon(uint64_t size) {
    vm_object_t *obj = vm_object_alloc(VM_OBJ_ANON, size);
    if (!obj)
        return NULL;
    obj->fault = vm_object_anon_fault;
    /* debug_printf("vm_object: created anon obj=%p size=0x%lx\r\n", obj, size); */
    return obj;
}

vm_object_t *vm_object_create_phys(uint64_t phys_base, uint64_t size) {
    vm_object_t *obj = vm_object_alloc(VM_OBJ_PHYS, size);
    if (!obj)
        return NULL;
    obj->phys.phys_base = phys_base;
    obj->fault = vm_object_phys_fault;
    /* debug_printf("vm_object: created phys obj=%p phys_base=0x%lx size=0x%lx\r\n",
                 obj, phys_base, size); */
    return obj;
}

vm_object_t *vm_object_create_shared(uint64_t size) {
    vm_object_t *obj = vm_object_alloc(VM_OBJ_SHARED, size);
    if (!obj)
        return NULL;
    obj->fault = vm_object_shared_fault;
    obj->shared.pages = NULL;
    obj->shared.capacity = 0;
    /* debug_printf("vm_object: created shared obj=%p size=0x%lx\r\n", obj, size); */
    return obj;
}

vm_object_t *vm_object_create_shadow(vm_object_t *parent) {
    if (!parent)
        return NULL;

    vm_object_t *obj = vm_object_alloc(VM_OBJ_SHADOW, parent->size);
    if (!obj)
        return NULL;

    obj->fault = vm_object_shadow_fault;
    obj->shadow.parent = parent;
    obj->shadow.map_nwords = (parent->size + PAGE_SIZE - 1) / PAGE_SIZE;
    obj->shadow.map_nwords = (obj->shadow.map_nwords + SHADOW_PAGE_BITS - 1) / SHADOW_PAGE_BITS;

    obj->shadow.copied_map = (unsigned long *)kcalloc(obj->shadow.map_nwords, sizeof(unsigned long));
    if (!obj->shadow.copied_map) {
        kfree(obj);
        return NULL;
    }

    vm_object_retain(parent);
    /* debug_printf("vm_object: created shadow obj=%p parent=%p size=0x%lx map_nwords=%d\r\n",
                 obj, parent, parent->size, obj->shadow.map_nwords); */
    return obj;
}

vm_object_t *vm_object_create_user_paged(uint64_t size, uint64_t pager_port_handle) {
    vm_object_t *obj = vm_object_alloc(VM_OBJ_USER_PAGED, size);
    if (!obj)
        return NULL;
    obj->paging_port = pager_port_handle;
    obj->fault = vm_object_user_paged_fault;
    /* debug_printf("vm_object: created user_paged obj=%p size=0x%lx port=0x%lx\r\n",
                 obj, size, pager_port_handle); */
    return obj;
}

vm_object_t *vm_object_retain(vm_object_t *obj) {
    if (!obj)
        return NULL;

    uint32_t flags;
    spin_lock_irqsave(&obj->lock, &flags);
    obj->ref_count++;
    spin_unlock_irqrestore(&obj->lock, flags);
    return obj;
}

void vm_object_release(vm_object_t *obj) {
    if (!obj)
        return;

    uint32_t flags;
    spin_lock_irqsave(&obj->lock, &flags);

    if (obj->ref_count == 0) {
        spin_unlock_irqrestore(&obj->lock, flags);
        return;
    }

    obj->ref_count--;
    int should_free = (obj->ref_count == 0);
    spin_unlock_irqrestore(&obj->lock, flags);

    if (!should_free)
        return;

    /* Ref count hit zero — clean up */
    if (obj->type == VM_OBJ_SHADOW) {
        if (obj->shadow.parent) {
            vm_object_release(obj->shadow.parent);
            obj->shadow.parent = NULL;
        }
        if (obj->shadow.copied_map) {
            kfree(obj->shadow.copied_map);
            obj->shadow.copied_map = NULL;
        }
    }

    if (obj->type == VM_OBJ_SHARED) {
        if (obj->shared.pages) {
            for (uint32_t i = 0; i < obj->shared.capacity; i++) {
                if (obj->shared.pages[i])
                    pmm_free_page((void *)(uintptr_t)obj->shared.pages[i]);
            }
            kfree(obj->shared.pages);
            obj->shared.pages = NULL;
        }
    }

    /* debug_printf("vm_object: releasing obj=%p type=%d\r\n", obj, obj->type); */
    kfree(obj);
}

/* ================================================================
 * Fault handlers
 * ================================================================ */

int vm_object_anon_fault(vm_object_t *obj, uint64_t offset,
                         vm_page_t *out_page, int write_fault) {
    (void)obj;
    (void)write_fault;

    if (offset >= obj->size)
        return -1;

    void *phys = pmm_alloc_page();
    if (!phys) {
        log_print(LOG_LEVEL_ERROR, "vm_object: anon_fault pmm_alloc_page failed\r\n");
        return -1;
    }

    void *va = vmm_temp_map((uint64_t)phys);
    if (!va) {
        pmm_free_page(phys);
        return -1;
    }

    memset(va, 0, PAGE_SIZE);
    vmm_temp_unmap();

    out_page->phys_addr = (uint64_t)phys;
    out_page->ref_count = 1;
    return 0;
}

int vm_object_phys_fault(vm_object_t *obj, uint64_t offset,
                         vm_page_t *out_page, int write_fault) {
    (void)write_fault;

    if (!obj || !out_page || offset >= obj->size)
        return -1;

    uint64_t page_offset = offset & ~(PAGE_SIZE - 1);
    out_page->phys_addr = obj->phys.phys_base + page_offset;
    out_page->ref_count = 1;
    return 0;
}

int vm_object_shared_fault(vm_object_t *obj, uint64_t offset,
                           vm_page_t *out_page, int write_fault) {
    (void)write_fault;

    if (!obj || !out_page || offset >= obj->size)
        return -1;

    uint64_t page_idx = offset / PAGE_SIZE;

    uint32_t flags;
    spin_lock_irqsave(&obj->lock, &flags);

    if (page_idx >= obj->shared.capacity) {
        uint32_t new_cap = obj->shared.capacity == 0 ? 64 : obj->shared.capacity * 2;
        while (new_cap <= page_idx)
            new_cap *= 2;

        uint64_t *new_pages = (uint64_t *)kcalloc(new_cap, sizeof(uint64_t));
        if (!new_pages) {
            spin_unlock_irqrestore(&obj->lock, flags);
            return -1;
        }

        if (obj->shared.pages) {
            memcpy(new_pages, obj->shared.pages, obj->shared.capacity * sizeof(uint64_t));
            kfree(obj->shared.pages);
        }
        obj->shared.pages = new_pages;
        obj->shared.capacity = new_cap;
    }

    if (obj->shared.pages[page_idx] != 0) {
        out_page->phys_addr = obj->shared.pages[page_idx];
        out_page->ref_count = 1;
        spin_unlock_irqrestore(&obj->lock, flags);
        return 0;
    }

    void *phys = pmm_alloc_page();
    if (!phys) {
        spin_unlock_irqrestore(&obj->lock, flags);
        return -1;
    }

    void *va = vmm_temp_map((uint64_t)phys);
    if (!va) {
        pmm_free_page(phys);
        spin_unlock_irqrestore(&obj->lock, flags);
        return -1;
    }
    memset(va, 0, PAGE_SIZE);
    vmm_temp_unmap();

    obj->shared.pages[page_idx] = (uint64_t)phys;
    out_page->phys_addr = (uint64_t)phys;
    out_page->ref_count = 1;

    spin_unlock_irqrestore(&obj->lock, flags);

    /* debug_printf("vm_object: shared_fault obj=%p offset=0x%lx -> new phys=0x%lx (cached at [%lu])\r\n",
                 obj, offset, (uint64_t)phys, page_idx); */
    return 0;
}

int vm_object_shadow_fault(vm_object_t *obj, uint64_t offset,
                           vm_page_t *out_page, int write_fault) {
    if (!obj || !out_page || offset >= obj->size || !obj->shadow.parent)
        return -1;

    uint64_t page_index = offset / PAGE_SIZE;
    int word_idx = page_index / SHADOW_PAGE_BITS;
    int bit_idx = page_index % SHADOW_PAGE_BITS;
    unsigned long mask = 1UL << bit_idx;

    uint32_t flags;
    spin_lock_irqsave(&obj->lock, &flags);

    /* Check if already copied */
    if (obj->shadow.copied_map[word_idx] & mask) {
        /* Already copied — need to find the physical page.
         * For simplicity, we fault the parent again (which will return
         * the same page if it's anon, or the phys page if phys).
         * A real implementation would cache the copied page. */
        spin_unlock_irqrestore(&obj->lock, flags);
        return obj->shadow.parent->fault(obj->shadow.parent, offset, out_page, write_fault);
    }

    /* Not copied yet — need to allocate a new page and copy from parent */
    vm_page_t parent_page;
    int ret = obj->shadow.parent->fault(obj->shadow.parent, offset, &parent_page, 0);
    if (ret < 0) {
        spin_unlock_irqrestore(&obj->lock, flags);
        return ret;
    }

    void *new_page = pmm_alloc_page();
    if (!new_page) {
        spin_unlock_irqrestore(&obj->lock, flags);
        return -1;
    }

    /* Map parent page temporarily to copy */
    void *parent_va = vmm_temp_map(parent_page.phys_addr);
    if (!parent_va) {
        pmm_free_page(new_page);
        spin_unlock_irqrestore(&obj->lock, flags);
        return -1;
    }

    memcpy(new_page, parent_va, PAGE_SIZE);
    vmm_temp_unmap();

    /* Mark as copied */
    obj->shadow.copied_map[word_idx] |= mask;
    spin_unlock_irqrestore(&obj->lock, flags);

    out_page->phys_addr = (uint64_t)new_page;
    out_page->ref_count = 1;

    /* debug_printf("vm_object: shadow_fault obj=%p offset=0x%lx -> new phys=0x%lx (copied from parent 0x%lx)\r\n",
                 obj, offset, out_page->phys_addr, parent_page.phys_addr); */
    return 0;
}

int vm_object_user_paged_fault(vm_object_t *obj, uint64_t offset,
                               vm_page_t *out_page, int write_fault) {
    if (!obj || !out_page || offset >= obj->size)
        return -1;

    uint32_t prot = VM_PROT_READ;
    if (write_fault)
        prot |= VM_PROT_WRITE;

    uint64_t phys_addr;
    int ret = vm_pager_send_fault(obj->paging_port, (uint64_t)obj, offset, prot, &phys_addr);
    if (ret < 0)
        return ret;

    out_page->phys_addr = phys_addr;
    out_page->ref_count = 1;

    /* debug_printf("vm_object: user_paged_fault obj=%p offset=0x%lx -> phys=0x%lx\r\n",
                 obj, offset, phys_addr); */
    return 0;
}