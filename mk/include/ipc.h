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


#ifndef IPC_ABI_H
#define IPC_ABI_H

/* ================================================================
 * ipc.h — arc IPC ABI
 *
 * This header defines the binary interface between kernel and
 * userspace for Inter-Process Communication over ports.
 *
 * It is shared between kernel and userspace: no kernel-private
 * types, no arch-specific features.
 *
 * ================================================================
 * Message ID allocation
 * ================================================================
 *
 * Every IPC message carries a 32-bit msg_id.  The space is
 * partitioned as follows:
 *
 *   0x0000            Invalid / reserved
 *   0x0001–0x00FF    Kernel-reserved system protocols
 *   0x0100–0x7FFF    Available for userspace protocols
 *   0x8000–0xFFFF    Kernel-reserved system protocols (high)
 *
 * Within the kernel-reserved ranges, allocations are:
 *
 *   0x8001           Pager fault request (faulting thread → pager)
 *   0x8002           Pager fault response (pager → faulting thread)
 *
 * ================================================================
 * Syscall numbers (mk-level)
 * ================================================================
 *
 * The kernel dispatches userspace syscalls through a flat number
 * space.  The IPC-related syscalls occupy:
 *
 *   SC_TASK_CREATE      13   — create a new task
 *   SC_TASK_DESTROY     14   — destroy current task
 *   SC_SLOT_ALLOC       15   — allocate a cspace slot
 *   SC_SLOT_FREE        16   — free a cspace slot
 *   SC_PORT_CREATE      17   — create a port, returns handle
 *   SC_PORT_DESTROY     18   — destroy a port by handle
 *   SC_PORT_SEND        19   — send a message (non-blocking)
 *   SC_PORT_RECV        20   — receive a message (blocking)
 *   SC_PORT_CALL        21   — send + block on reply
 *   SC_PORT_REPLY       22   — reply to a call
 *   SC_PORT_NOTIFY      23   — send one-bit notification
 *   SC_PORT_POLL        24   — poll multiple ports
 *   SC_VM_CREATE_SHARED 25   — create shared memory object
 *   SC_VM_CREATE_PHYS   26   — create physical memory object
 *   SC_VM_MAP           27   — map memory into address space
 *   SC_VM_UNMAP         28   — unmap memory
 *   SC_VM_PROTECT       29   — change memory protection
 *   SC_BSD_BASE        1024  — BSD syscalls start here
 *
 * ================================================================
 * Capability types
 * ================================================================
 *
 * Capabilities identify kernel objects in C-space slots.
 * Types (must match task.h):
 *
 *   CAP_PORT      1   — IPC port
 *   CAP_MEMORY    2   — memory object
 *   CAP_THREAD    3   — thread
 *
 * Rights (bitmask):
 *
 *   CAP_SEND      (1<<0) — send / transfer capability
 *   CAP_RECV      (1<<1) — receive from port
 *   CAP_REPLY     (1<<2) — reply to a call
 *   CAP_READ      (1<<3) — read memory
 *   CAP_WRITE     (1<<4) — write memory
 *   CAP_EXEC      (1<<5) — execute memory
 *
 * ================================================================
 * Data format
 * ================================================================
 *
 * Message payload (ipc_msg_t.data[64]) is protocol-defined.
 * All multi-byte fields are little-endian.
 * `data_size` must equal sizeof(the_request_struct).
 * Extending: add fields at the end, bump a version field or use a
 * new msg_id.  Receiver checks data_size >= expected.
 *
 * ================================================================
 * Pager fault protocol
 * ================================================================
 *
 * A USER_PAGED vm_object delegates page faults to a pager thread
 * via IPC.  The protocol is:
 *
 *   1. Faulting thread sends IPC_FAULT_REQ to the pager port.
 *   2. Pager thread receives IPC_FAULT_REQ, resolves the address,
 *      and calls vm_pager_handle_reply() to unblock the faulting
 *      thread with the resolved physical address.
 *      (The reply is a kernel-internal call, not an IPC message.)
 *
 * This protocol is owned by the kernel.  Userspace pager
 * implementations that replace the boot pager must follow the
 * same wire format for IPC_FAULT_REQ.
 *
 * Layout of ipc_msg_t.data for IPC_FAULT_REQ (36 bytes):
 *
 *   offset  size  field
 *   ------  ----  -------------------------
 *        0     8  object_id       (vm_object identifier)
 *        8     8  fault_offset    (byte offset within object)
 *       16     4  prot            (VM_PROT_READ/WRITE/EXEC)
 *       20     4  fault_tid       (TID of blocked thread)
 *       24     8  reply_handle    (port handle for response)
 *       32     4  _reserved       (pad to 36 bytes)
 *   ================================================================
 */

#include <stdint.h>

/* ---- Message ID ranges ---- */
#define IPC_ID_INVALID          0x0000

#define IPC_ID_KERNEL_BASE      0x0001
#define IPC_ID_KERNEL_END       0x00FF

#define IPC_ID_USER_BASE        0x0100
#define IPC_ID_USER_END         0x7FFF

#define IPC_ID_KERNEL_HIGH_BASE 0x8000
#define IPC_ID_KERNEL_HIGH_END  0xFFFF

/* ---- Standard message IDs ---- */
#define IPC_FAULT_REQ           0x8001   /* Page fault request  */
#define IPC_FAULT_RESP          0x8002   /* Page fault response */

/* ---- Syscall numbers (mk-level) ---- */
#define SC_TASK_CREATE          13
#define SC_TASK_DESTROY         14
#define SC_SLOT_ALLOC           15
#define SC_SLOT_FREE            16

#define SC_PORT_CREATE          17
#define SC_PORT_DESTROY         18
#define SC_PORT_SEND            19
#define SC_PORT_RECV            20
#define SC_PORT_CALL            21
#define SC_PORT_REPLY           22
#define SC_PORT_NOTIFY          23
#define SC_PORT_POLL            24

#define SC_VM_CREATE_SHARED     25
#define SC_VM_CREATE_PHYS       26
#define SC_VM_MAP               27
#define SC_VM_UNMAP             28
#define SC_VM_PROTECT           29

#define SC_BSD_BASE             1024

/* ---- Capability types (mirrors task.h) ---- */
#define CAP_PORT                1
#define CAP_MEMORY              2
#define CAP_THREAD              3

/* ---- Capability rights (mirrors task.h) ---- */
#define CAP_SEND                (1u << 0)
#define CAP_RECV                (1u << 1)
#define CAP_REPLY               (1u << 2)
#define CAP_READ                (1u << 3)
#define CAP_WRITE               (1u << 4)
#define CAP_EXEC                (1u << 5)

/* ================================================================
 * Pager fault protocol structures
 *
 * These structs are packed and overlay ipc_msg_t.data.
 * ================================================================ */

/* IPC_FAULT_REQ payload — faulting thread → pager */
typedef struct __attribute__((packed)) {
    uint64_t object_id;         /* vm_object identifier            */
    uint64_t fault_offset;      /* byte offset within object       */
    uint32_t prot;              /* VM_PROT_READ | VM_PROT_WRITE   */
    uint32_t fault_tid;         /* TID of the blocked thread      */
    uint64_t reply_handle;      /* Capability handle for reply    */
} ipc_pager_fault_req_t;

/* IPC_FAULT_RESP payload — pager → faulting thread */
typedef struct __attribute__((packed)) {
    uint64_t phys_addr;         /* Resolved physical address      */
} ipc_pager_fault_resp_t;

_Static_assert(sizeof(ipc_pager_fault_req_t) <= 64,
               "ipc_pager_fault_req_t exceeds IPC_DATA_SIZE (64)");
_Static_assert(sizeof(ipc_pager_fault_resp_t) <= 64,
               "ipc_pager_fault_resp_t exceeds IPC_DATA_SIZE (64)");

/* ---- Boot-time self-test ---- */
void ipc_abi_test(void);

#endif /* IPC_ABI_H */
