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


#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include "spinlock.h"
#include "task.h"

/* ================================================================
 * port.h — Capability-based IPC ports for the arc kernel.
 *
 * Ports are the sole IPC primitive.  Messages can carry inline data
 * plus up to IPC_DISPOSE_MAX capabilities (ports, memory, threads).
 * dispose = move semantics: the sender loses the capability.
 *
 * notify() is a lightweight single-bit signal (no queue, no data).
 * poll() waits on multiple ports simultaneously (like select for ports).
 * ================================================================ */

#define IPC_DATA_SIZE      64
#define IPC_DISPOSE_MAX    4
#define PORT_DEFAULT_QUEUE_SIZE  128  /* dynamic, grown on demand */
#define PORT_WAITERS_MAX   8          /* blocked recv/poll threads per port */

/* ---- IPC error codes ---- */
#define IPC_OK         0
#define IPC_ERR_NOSLOT  -1
#define IPC_ERR_FULL    -2
#define IPC_ERR_EMPTY   -3
#define IPC_ERR_TIMEOUT -4
#define IPC_ERR_FAULT   -5
#define IPC_ERR_INVAL   -6

/* ---- Capability dispose descriptor ---- */
typedef struct ipc_dispose {
    uint32_t type;          /* CAP_PORT | CAP_MEMORY | CAP_THREAD */
    uint32_t rights;
    uint64_t handle;        /* source handle in sender's cspace */
} ipc_dispose_t;

/* ---- IPC message ---- */
typedef struct ipc_msg {
    uint32_t       msg_id;
    uint32_t       data_size;
    uint8_t        data[IPC_DATA_SIZE];
    uint32_t       ndispose;
    ipc_dispose_t  dispose[IPC_DISPOSE_MAX];
} ipc_msg_t;

/* ---- Port object ---- */
typedef struct ipc_port {
    uint32_t   magic;          /* 0x504F5254 — corruption canary */
    int        ref_count;      /* references from cspace slots; mutated
                                 only under port_table_lock (see below) */
    spinlock_t lock;

    /* Circular message queue (dynamically allocated, grows on demand) */
    ipc_msg_t  *queue;
    uint32_t   *queue_senders;  /* per-slot sender task_id (kernel-only) */
    uint32_t    queue_size;     /* allocated slot count */
    uint32_t    queue_head;
    uint32_t    queue_tail;
    uint32_t    queue_count;

    /* Pending notify flag (single bit, cleared on first recv) */
    int        pending_notify;

    /* Threads blocked in recv/poll on this port.  A single blocked_tid
     * lost wakeups whenever two threads received from one port; the
     * list is bounded and deduplicated.  Manipulated under ->lock. */
    uint32_t   waiters[PORT_WAITERS_MAX];
    int        waiter_count;
} ipc_port_t;

/* ================================================================
 * Port kernel-internal API
 * ================================================================ */

ipc_port_t *port_create(void);
/* Mark the port dead, wake its waiters, drop one reference (the
 * creator's).  The memory is freed when the last slot reference goes
 * away via port_release(). */
int         port_destroy(ipc_port_t *port);
/* Drop one reference taken by a lookup; frees the port at zero.
 * Safe to call with a stale pointer: membership in the global port
 * table is verified first. */
void        port_release(ipc_port_t *port);
int         port_send(ipc_port_t *port, const ipc_msg_t *msg, task_t *sender);
int         port_recv(ipc_port_t *port, ipc_msg_t *msg);
int         port_notify(ipc_port_t *port);

/* ================================================================
 * Syscalls
 * ================================================================ */

int sys_port_create(void);              /* returns slot handle (task_id<<32|slot) */
int sys_port_destroy(uint64_t handle);
int sys_port_send(uint64_t handle, const ipc_msg_t *user_msg);
int sys_port_recv(uint64_t handle, ipc_msg_t *user_msg);
int sys_port_call(uint64_t handle, ipc_msg_t *user_msg);
int sys_port_reply(uint64_t reply_handle, const ipc_msg_t *user_msg);
int sys_port_notify(uint64_t handle);
int sys_port_poll(const uint64_t *handles, int count, uint64_t timeout_us);

#endif /* PORT_H */
