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


#include <stdint.h>
#include <stddef.h>
#include "vm_object.h"
#include "port.h"
#include "task.h"
#include "thread.h"
#include "scheduler.h"
#include "spinlock.h"
#include "debug.h"
#include "string.h"
#include "pmm.h"
/* errno definitions — bsd/include/bsd/errno.h */
#define EAGAIN  11
#define EINVAL  22
#define ENOMEM  12
#define ESRCH   3

/* ================================================================
 * vm_pager.c — User-space pager IPC protocol
 *
 * Implements the kernel side of the pager protocol:
 *   - vm_pager_send_fault(): called from vm_object_user_paged_fault()
 *     to send a page fault IPC to the user-space pager and block
 *     the faulting thread.
 *   - vm_pager_handle_reply(): called from syscall handler when a
 *     pager thread replies with IPC_FAULT_RESP; unblocks the
 *     faulting thread and returns the resolved physical address.
 * ================================================================ */

#define PAGER_FAULTS_MAX 16

/* ---- Pending pager fault table ---- */
typedef struct pager_fault {
    uint32_t blocked_tid;   /* TID of the thread blocked on this fault */
    uint64_t reply_phys;    /* Physical address from pager reply */
    int      valid;         /* Entry in use */
} pager_fault_t;

static pager_fault_t pager_faults[PAGER_FAULTS_MAX];
static spinlock_t pager_faults_lock = SPINLOCK_INIT;

/* ---- Internal: find a free pager_fault slot ---- */
static int pager_fault_alloc_slot(void) {
    for (int i = 0; i < PAGER_FAULTS_MAX; i++) {
        if (!pager_faults[i].valid) {
            pager_faults[i].valid = 1;
            return i;
        }
    }
    return -1;
}

/* ---- Internal: find slot by blocked_tid ---- */
static int pager_fault_find_by_tid(uint32_t tid) {
    for (int i = 0; i < PAGER_FAULTS_MAX; i++) {
        if (pager_faults[i].valid && pager_faults[i].blocked_tid == tid) {
            return i;
        }
    }
    return -1;
}

/* ---- Internal: resolve a paging port handle to an ipc_port_t* ---- */
static ipc_port_t *pager_resolve_port(uint64_t handle) {
    if (!handle)
        return NULL;

    uint32_t task_id = handle_task_id(handle);
    int slot = handle_slot(handle);

    task_t *t = task_find(task_id);
    if (!t)
        return NULL;

    cslot_t *cs = cspace_lookup(&t->cspace, slot);
    if (!cs || cs->type != CAP_PORT)
        return NULL;
    if (!(cs->rights & CAP_SEND))
        return NULL;

    return (ipc_port_t *)(uintptr_t)cs->object_id;
}

/* ================================================================
 * vm_pager_send_fault
 *
 * Send a page fault IPC to the user-space pager and block the
 * current thread until the pager replies.
 *
 * Parameters:
 *   paging_port_handle - capability handle for the pager port
 *   object_id          - vm_object pointer (as uint64_t) for identification
 *   fault_offset       - byte offset within the object
 *   prot               - protection flags (VM_PROT_READ | VM_PROT_WRITE)
 *   out_phys           - output: physical address from pager reply
 *
 * Returns:
 *   0 on success, *out_phys set to the physical address.
 *   -EAGAIN if pager port queue is full (caller should retry).
 *   -EINVAL if handle is invalid or port lookup fails.
 *   -ENOMEM if no free pager_fault slot or reply port allocation fails.
 * ================================================================ */
int vm_pager_send_fault(uint64_t paging_port_handle,
                        uint64_t object_id,
                        uint64_t fault_offset,
                        uint32_t prot,
                        uint64_t *out_phys) {
    if (!out_phys)
        return -EINVAL;

    /* Resolve the pager port handle */
    ipc_port_t *pager_port = pager_resolve_port(paging_port_handle);
    if (!pager_port) {
        debug_print("vm_pager: invalid paging port handle\r\n");
        return -EINVAL;
    }

    /* Allocate a slot to track this pending fault */
    uint32_t flags;
    spin_lock_irqsave(&pager_faults_lock, &flags);
    int slot = pager_fault_alloc_slot();
    spin_unlock_irqrestore(&pager_faults_lock, flags);

    if (slot < 0) {
        debug_print("vm_pager: no free pager_fault slots\r\n");
        return -ENOMEM;
    }

    /* Create a temporary reply port for the pager to respond to */
    ipc_port_t *reply_port = port_create();
    if (!reply_port) {
        spin_lock_irqsave(&pager_faults_lock, &flags);
        pager_faults[slot].valid = 0;
        spin_unlock_irqrestore(&pager_faults_lock, flags);
        debug_print("vm_pager: port_create failed for reply port\r\n");
        return -ENOMEM;
    }

    /* Allocate a handle for the reply port in the current task's cspace */
    task_t *cur_task = task_current();
    if (!cur_task) {
        port_destroy(reply_port);
        spin_lock_irqsave(&pager_faults_lock, &flags);
        pager_faults[slot].valid = 0;
        spin_unlock_irqrestore(&pager_faults_lock, flags);
        return -EINVAL;
    }

    int reply_slot = cspace_alloc_slot(&cur_task->cspace, CAP_PORT,
                                       CAP_RECV | CAP_REPLY,
                                       (uint64_t)(uintptr_t)reply_port);
    if (reply_slot < 0) {
        port_destroy(reply_port);
        spin_lock_irqsave(&pager_faults_lock, &flags);
        pager_faults[slot].valid = 0;
        spin_unlock_irqrestore(&pager_faults_lock, flags);
        debug_print("vm_pager: cspace_alloc_slot failed for reply port\r\n");
        return -ENOMEM;
    }
    uint64_t reply_handle = task_make_handle(cur_task->task_id, reply_slot);

    /* Get current thread for fault tracking */
    thread_t *cur_thread = thread_current();
    if (!cur_thread) {
        cspace_free_slot(&cur_task->cspace, reply_slot);
        port_destroy(reply_port);
        spin_lock_irqsave(&pager_faults_lock, &flags);
        pager_faults[slot].valid = 0;
        spin_unlock_irqrestore(&pager_faults_lock, flags);
        return -EINVAL;
    }

    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_id = IPC_FAULT_REQ;

    ipc_pager_fault_req_t *req = (ipc_pager_fault_req_t *)msg.data;
    req->object_id    = object_id;
    req->fault_offset = fault_offset;
    req->prot         = prot;
    req->fault_tid    = cur_thread->tid;
    req->reply_handle = reply_handle;
    msg.data_size = sizeof(ipc_pager_fault_req_t);
    msg.ndispose = 0;

    /* Send the fault request to the pager port */
    int ret = port_send(pager_port, &msg, cur_task);
    if (ret != IPC_OK) {
        /* Clean up on send failure */
        cspace_free_slot(&cur_task->cspace, reply_slot);
        port_destroy(reply_port);
        spin_lock_irqsave(&pager_faults_lock, &flags);
        pager_faults[slot].valid = 0;
        spin_unlock_irqrestore(&pager_faults_lock, flags);

        if (ret == IPC_ERR_FULL) {
            debug_print("vm_pager: pager port queue full\r\n");
            return -EAGAIN;
        }
        debug_printf("vm_pager: port_send failed: %d\r\n", ret);
        return -EINVAL;
    }

    /* Record the faulting thread and reply port handle */
    spin_lock_irqsave(&pager_faults_lock, &flags);
    pager_faults[slot].blocked_tid = cur_thread->tid;
    pager_faults[slot].reply_phys = 0;
    spin_unlock_irqrestore(&pager_faults_lock, flags);

    /* Block the current thread */
    cur_thread->state = THREAD_BLOCKED;

    thread_yield();

    /* Thread has been woken by vm_pager_handle_reply */
    spin_lock_irqsave(&pager_faults_lock, &flags);
    uint64_t phys = pager_faults[slot].reply_phys;
    pager_faults[slot].valid = 0;
    spin_unlock_irqrestore(&pager_faults_lock, flags);

    /* Clean up the reply port handle (no longer needed) */
    cspace_free_slot(&cur_task->cspace, reply_slot);
    port_destroy(reply_port);

    if (phys == 0) {
        debug_print("vm_pager: pager replied with zero phys_addr\r\n");
        return -EINVAL;
    }

    *out_phys = phys;
    debug_printf("vm_pager: fault resolved for tid=%u obj=0x%lx offset=0x%lx -> phys=0x%lx\r\n",
                 cur_thread->tid, object_id, fault_offset, phys);
    return 0;
}

/* ================================================================
 * vm_pager_handle_reply
 *
 * Called from the syscall handler when a pager thread sends an
 * IPC_FAULT_RESP reply. Finds the blocked faulting thread,
 * stores the physical address, and unblocks it.
 *
 * Parameters:
 *   fault_tid  - TID of the thread that faulted (from the reply message)
 *   phys_addr  - Physical address provided by the pager
 *
 * Returns:
 *   0 on success, -ESRCH if no matching fault_tid found.
 * ================================================================ */
int vm_pager_handle_reply(uint64_t fault_tid, uint64_t phys_addr) {
    if (fault_tid == 0 || phys_addr == 0)
        return -EINVAL;

    uint32_t flags;
    spin_lock_irqsave(&pager_faults_lock, &flags);

    int slot = pager_fault_find_by_tid((uint32_t)fault_tid);
    if (slot < 0) {
        spin_unlock_irqrestore(&pager_faults_lock, flags);
        debug_printf("vm_pager: no pending fault for tid=%lu\r\n", fault_tid);
        return -ESRCH;
    }

    /* Store the physical address from the pager */
    pager_faults[slot].reply_phys = phys_addr;

    /* Find and unblock the faulting thread */
    thread_t *fault_thread = thread_find((uint32_t)fault_tid);
    if (!fault_thread) {
        spin_unlock_irqrestore(&pager_faults_lock, flags);
        debug_printf("vm_pager: thread_find failed for tid=%lu\r\n", fault_tid);
        return -ESRCH;
    }

    if (fault_thread->state != THREAD_BLOCKED) {
        debug_printf("vm_pager: fault thread tid=%u not BLOCKED (state=%d)\r\n",
                     fault_thread->tid, fault_thread->state);
    }

    fault_thread->state = THREAD_READY;
    scheduler_add_thread(fault_thread);

    spin_unlock_irqrestore(&pager_faults_lock, flags);

    debug_printf("vm_pager: reply for tid=%lu phys=0x%lx, thread unblocked\r\n",
                 fault_tid, phys_addr);
    return 0;
}

/* ================================================================
 * Boot-time kernel pager
 *
 * A simple kernel thread pager used during early boot to resolve
 * page faults for USER_PAGED objects.  Creates a port and loops
 * receiving IPC_FAULT_REQ messages, allocating physical pages, and
 * replying via vm_pager_handle_reply.
 * ================================================================ */

/* The pager port — exported so main.c can wire USER_PAGED objects */
ipc_port_t *boot_pager_port;

static void boot_pager_entry(void) {
    ipc_port_t *port = port_create();
    if (!port) {
        debug_print("boot_pager: port_create failed\n");
        return;
    }
    boot_pager_port = port;

    ipc_msg_t msg;
    while (1) {
        memset(&msg, 0, sizeof(msg));
        int ret = port_recv(port, &msg);

        if (ret == IPC_ERR_EMPTY) {
            /* Queue empty — thread is now BLOCKED.  Yield and retry. */
            thread_yield();
            continue;
        }

        if (ret != IPC_OK) {
            continue;
        }

        if (msg.msg_id != IPC_FAULT_REQ) {
            continue;
        }

        ipc_pager_fault_req_t *req = (ipc_pager_fault_req_t *)msg.data;
        (void)req->object_id;
        (void)req->prot;
        (void)req->reply_handle;

        /* Allocate a physical page */
        void *phys = pmm_alloc_page();
        if (!phys) {
            debug_print("boot_pager: out of memory\n");
            continue;
        }

        /* Resolve the fault — unblock the faulting thread */
        vm_pager_handle_reply(req->fault_tid, (uint64_t)phys);
    }
}

/* Exposed so mk_init (main.c) can create the pager thread */
void boot_pager_init(void) {
    thread_t *thr = thread_create((uint64_t)boot_pager_entry, NULL, 0);
    if (!thr) {
        debug_print("boot_pager: thread_create failed\n");
        return;
    }
    scheduler_add_thread(thr);
}