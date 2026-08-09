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
#include "vm_object.h"
#include "vmm.h"
#include "debug.h"
#include "string.h"

/* ================================================================
 * vm_syscall.c — Phase 3 VM syscalls
 *
 * Exposes vm_object + vm_map to userspace through capability
 * handles (CAP_MEMORY slots in C-space).
 *
 * Syscall numbers (next available after Phase 1 IPC, which ends at 24):
 *   25 — sys_vm_create_shared(size)     → returns handle
 *   26 — sys_vm_create_phys(phys, size) → returns handle
 *   27 — sys_vm_map(handle, addr, prot) → 0 / -1
 *   28 — sys_vm_unmap(addr, size)       → 0 / -1
 *   29 — sys_vm_protect(addr, size, prot) → 0 / -1
 * ================================================================ */

/* ---- Syscall 25 ---- */
int sys_vm_create_shared(uint64_t size) {
    task_t *cur = task_current();
    if (!cur) return -1;

    /* Round up to page boundary */
    if (size == 0) return -1;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    vm_object_t *obj = vm_object_create_shared(size);
    if (!obj) return -1;

    int slot = cspace_alloc_slot(&cur->cspace, CAP_MEMORY,
                                  CAP_READ | CAP_WRITE,
                                  (uint64_t)(uintptr_t)obj);
    if (slot < 0) {
        vm_object_release(obj);
        return -1;
    }

    debug_printf("vm_syscall: create_shared size=0x%lx -> handle=0x%lx (task=%u slot=%d)\r\n",
                 size, task_make_handle(cur->task_id, slot), cur->task_id, slot);
    return (int)task_make_handle(cur->task_id, slot);
}

/* ---- Syscall 26 ---- */
int sys_vm_create_phys(uint64_t phys_base, uint64_t size) {
    task_t *cur = task_current();
    if (!cur) return -1;

    if (size == 0) return -1;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    vm_object_t *obj = vm_object_create_phys(phys_base, size);
    if (!obj) return -1;

    int slot = cspace_alloc_slot(&cur->cspace, CAP_MEMORY,
                                  CAP_READ | CAP_WRITE,
                                  (uint64_t)(uintptr_t)obj);
    if (slot < 0) {
        vm_object_release(obj);
        return -1;
    }

    debug_printf("vm_syscall: create_phys phys=0x%lx size=0x%lx -> handle=0x%lx\r\n",
                 phys_base, size, task_make_handle(cur->task_id, slot));
    return (int)task_make_handle(cur->task_id, slot);
}

/* ---- Internal: resolve a CAP_MEMORY handle to a vm_object_t* ---- */
static vm_object_t *handle_to_vm_object(uint64_t handle) {
    uint32_t tid = handle_task_id(handle);
    int slot = handle_slot(handle);

    task_t *t = task_find(tid);
    if (!t) return NULL;

    cslot_t *cs = cspace_lookup(&t->cspace, slot);
    if (!cs || cs->type != CAP_MEMORY) return NULL;

    return (vm_object_t *)(uintptr_t)cs->object_id;
}

/* ---- Syscall 27 ---- */
int sys_vm_map(uint64_t handle, uint64_t addr, uint32_t prot) {
    task_t *cur = task_current();
    if (!cur || !cur->map) return -1;

    if (addr & (PAGE_SIZE - 1)) {
        debug_print("vm_syscall: map addr not page-aligned\r\n");
        return -1;
    }

    vm_object_t *obj = handle_to_vm_object(handle);
    if (!obj) {
        debug_print("vm_syscall: map invalid handle\r\n");
        return -1;
    }

    /* Only respect READ/WRITE/EXEC from userspace */
    prot &= VM_PROT_ALL;

    int ret = vm_map_map(cur->map, addr, obj, 0, obj->size, prot, VM_INHERIT_SHARE);
    if (ret < 0) {
        debug_printf("vm_syscall: map failed for handle=0x%lx addr=0x%lx prot=0x%x\r\n",
                     handle, addr, prot);
        return -1;
    }

    debug_printf("vm_syscall: map handle=0x%lx at 0x%lx size=0x%lx prot=0x%x\r\n",
                 handle, addr, obj->size, prot);
    return 0;
}

/* ---- Syscall 28 ---- */
int sys_vm_unmap(uint64_t addr, uint64_t size) {
    task_t *cur = task_current();
    if (!cur || !cur->map) return -1;

    if (addr & (PAGE_SIZE - 1)) return -1;
    if (size == 0) return -1;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    int ret = vm_map_unmap(cur->map, addr, size);
    debug_printf("vm_syscall: unmap addr=0x%lx size=0x%lx -> %d\r\n", addr, size, ret);
    return ret;
}

/* ---- Syscall 29 ---- */
int sys_vm_protect(uint64_t addr, uint64_t size, uint32_t prot) {
    task_t *cur = task_current();
    if (!cur || !cur->map) return -1;

    if (addr & (PAGE_SIZE - 1)) return -1;
    if (size == 0) return -1;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    prot &= VM_PROT_ALL;

    int ret = vm_map_protect(cur->map, addr, size, prot);
    debug_printf("vm_syscall: protect addr=0x%lx size=0x%lx prot=0x%x -> %d\r\n",
                 addr, size, prot, ret);
    return ret;
}
