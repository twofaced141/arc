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


#ifndef VM_OBJECT_H
#define VM_OBJECT_H

#include <stdint.h>
#include "spinlock.h"
#include "ipc.h"

struct ipc_port;  /* forward decl, full type in port.h */

/* ================================================================
 * vm_object.h — VM object + address space abstractions
 *
 * vm_object is the unified memory abstraction.  Every page of memory
 * in a task's address space belongs to exactly one vm_object.
 * Page faults are resolved by calling obj->fault(), which either
 * allocates a page (ANON), returns a pre-allocated one (PHYS),
 * copies-on-write (SHADOW), or sends an IPC to a user-space pager
 * (USER_PAGED).
 *
 * vm_map is a task's address space: a set of vm_entry ranges that
 * each map a portion of a vm_object into the task's virtual address
 * space.
 * ================================================================ */

/* ---- Forward declarations ---- */
struct vm_object;
struct vm_map;

/* ---- Page descriptor ---- */
typedef struct vm_page {
    uint64_t phys_addr;
    int      ref_count;
} vm_page_t;

/* ---- Object types ---- */
#define VM_OBJ_ANON         1   /* anonymous memory, alloc on fault */
#define VM_OBJ_PHYS         2   /* fixed physical memory */
#define VM_OBJ_SHARED       3   /* shared across tasks */
#define VM_OBJ_SHADOW       4   /* COW delta over a parent object */
#define VM_OBJ_USER_PAGED   5   /* faults go to user-space pager */
#define VM_OBJ_VNODE        6   /* file-backed (future) */

/* ---- Protection flags ---- */
#define VM_PROT_NONE    0
#define VM_PROT_READ    (1u << 0)
#define VM_PROT_WRITE   (1u << 1)
#define VM_PROT_EXEC    (1u << 2)
#define VM_PROT_ALL     (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)

/* ---- Inheritance flags (for fork) ---- */
#define VM_INHERIT_SHARE    0
#define VM_INHERIT_COPY     1
#define VM_INHERIT_NONE     2

/* ================================================================
 * vm_object
 * ================================================================ */

/* Shadow-object internal: bitmap of already-copied pages */
#define SHADOW_PAGE_BITS    (8 * sizeof(unsigned long))

typedef struct vm_object_shadow {
    struct vm_object *parent;
    unsigned long    *copied_map;   /* bitmap: 1 = already copied */
    int              map_nwords;    /* number of unsigned longs in copied_map */
} vm_object_shadow_t;

typedef struct vm_object_phys {
    uint64_t phys_base;
} vm_object_phys_t;

typedef struct vm_object_anon {
    /* nothing special (yet) */
    int dummy;
} vm_object_anon_t;

typedef struct vm_object_shared {
    uint64_t *pages;     /* page cache: phys_addr per page index, 0 = unallocated */
    uint32_t  capacity;  /* allocated entries in pages[] */
} vm_object_shared_t;

/* Fault function signature.
 * Given an object and a byte offset, return a vm_page with the resolved
 * physical page.
 * Returns 0 on success, negative on error.
 * For USER_PAGED objects, returns -EAGAIN while waiting for the pager;
 * the thread is blocked and will be woken when the pager replies. */
typedef int (*vm_fault_fn_t)(struct vm_object *obj, uint64_t offset,
                             vm_page_t *out_page, int write_fault);

typedef struct vm_object {
    int             ref_count;
    int             type;         /* VM_OBJ_ANON | _PHYS | _SHARED | _SHADOW | _USER_PAGED */
    uint64_t        size;         /* size in bytes */
    uint64_t        paging_port;  /* handle of pager port (USER_PAGED only) */
    spinlock_t      lock;

    vm_fault_fn_t   fault;

    union {
        vm_object_anon_t    anon;
        vm_object_phys_t    phys;
        vm_object_shared_t  shared;
        vm_object_shadow_t  shadow;
    };
} vm_object_t;

/* ================================================================
 * vm_entry + vm_map
 * ================================================================ */

typedef struct vm_entry {
    uint64_t       start;
    uint64_t       end;
    vm_object_t   *obj;
    uint64_t       obj_offset;   /* offset within the object */
    uint32_t       prot;         /* VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC */
    uint32_t       inherit;      /* VM_INHERIT_SHARE | _COPY | _NONE */
} vm_entry_t;

/* Forward-declare page_directory_t from vmm.h */
struct page_directory;

typedef struct vm_map {
    vm_entry_t            *entries;      /* dynamically allocated array */
    int                   entry_count;
    int                   max_entries;   /* allocated capacity */
    struct page_directory *pml4;         /* arch page tables */
    spinlock_t            lock;
} vm_map_t;

/* ================================================================
 * vm_object API
 * ================================================================ */

void            vm_object_init(void);
void            vm_object_test_shared(void);  /* self-test, called during boot */

vm_object_t    *vm_object_create_anon(uint64_t size);
vm_object_t    *vm_object_create_phys(uint64_t phys_base, uint64_t size);
vm_object_t    *vm_object_create_shared(uint64_t size);
vm_object_t    *vm_object_create_shadow(vm_object_t *parent);
vm_object_t    *vm_object_create_user_paged(uint64_t size, uint64_t pager_port_handle);

vm_object_t    *vm_object_retain(vm_object_t *obj);
void            vm_object_release(vm_object_t *obj);

/* Default fault handlers (public for pager.c / shadow.c) */
int vm_object_anon_fault(vm_object_t *obj, uint64_t offset,
                         vm_page_t *out_page, int write_fault);
int vm_object_phys_fault(vm_object_t *obj, uint64_t offset,
                         vm_page_t *out_page, int write_fault);
int vm_object_shared_fault(vm_object_t *obj, uint64_t offset,
                           vm_page_t *out_page, int write_fault);
int vm_object_shadow_fault(vm_object_t *obj, uint64_t offset,
                           vm_page_t *out_page, int write_fault);
int vm_object_user_paged_fault(vm_object_t *obj, uint64_t offset,
                               vm_page_t *out_page, int write_fault);

/* ================================================================
 * vm_map API
 * ================================================================ */

int     vm_map_init(vm_map_t *map, struct page_directory *pml4);
void    vm_map_destroy(vm_map_t *map);

int     vm_map_map(vm_map_t *map, uint64_t addr, vm_object_t *obj,
                   uint64_t obj_offset, uint64_t size, uint32_t prot,
                   uint32_t inherit);
int     vm_map_unmap(vm_map_t *map, uint64_t addr, uint64_t size);
int     vm_map_protect(vm_map_t *map, uint64_t addr, uint64_t size,
                       uint32_t prot);

vm_entry_t *vm_map_find_entry(vm_map_t *map, uint64_t addr);

/* Resolve a page fault through the vm_map.
 * Returns 0 if resolved, negative if SIGSEGV.
 * On success 'out_phys' is set to the physical address to map. */
int     vm_map_resolve_fault(vm_map_t *map, uint64_t fault_addr,
                             int write_fault, uint64_t *out_phys);

/* ================================================================
 * Pager API (vm_pager.c)
 * ================================================================ */

/* Called from the USER_PAGED fault handler to send a fault IPC
 * to the pager and block.  Returns 0 when the pager has replied
 * and 'out_phys' contains the physical address to map. */
int vm_pager_send_fault(uint64_t paging_port_handle,
                        uint64_t object_id,
                        uint64_t fault_offset,
                        uint32_t prot,
                        uint64_t *out_phys);

/* Called from the syscall handler when a pager thread calls
 * port_reply with IPC_FAULT_RESP.  Unblocks the faulting thread. */
int vm_pager_handle_reply(uint64_t fault_tid, uint64_t phys_addr);

extern struct ipc_port *boot_pager_port;
void boot_pager_init(void);

/* ================================================================
 * Phase 3 VM syscalls (implemented in vm_syscall.c)
 * ================================================================ */

/* Syscall 25 — create a shared memory object, returns handle */
int sys_vm_create_shared(uint64_t size);

/* Syscall 26 — create a physical memory object (for drivers), returns handle */
int sys_vm_create_phys(uint64_t phys_base, uint64_t size);

/* Syscall 27 — map a CAP_MEMORY handle into current task's address space */
int sys_vm_map(uint64_t handle, uint64_t addr, uint32_t prot);

/* Syscall 28 — unmap a region from current task's address space */
int sys_vm_unmap(uint64_t addr, uint64_t size);

/* Syscall 29 — change protection on a mapped region */
int sys_vm_protect(uint64_t addr, uint64_t size, uint32_t prot);

#endif /* VM_OBJECT_H */
