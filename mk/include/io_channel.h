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


#ifndef IO_CHANNEL_H
#define IO_CHANNEL_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

/* ================================================================
 * io_channel.h — Generic I/O Request Channel
 *
 * A request queue that lets kernel subsystems delegate I/O work to
 * userspace drivers.  The pattern is:
 *
 *   kernel code → io_channel_request(ch, &req) → [enqueue, spin]
 *                                                   ↓
 *   userspace driver ← sys_io_get_request()        |
 *                   → AHCI DMA / MMIO              |
 *                   → sys_io_complete()  ──────────→ [wake caller]
 *
 * Opcodes and arg[] are device-specific; the channel itself is
 * opcode-agnostic.  New driver types (NVMe, e1000, input) reuse
 * the same channel + syscall interface.
 * ================================================================ */

/* ---- Request descriptor ---- */

#define IO_REQ_MAX_ARG  4
#define IO_REQ_BUF_SIZE 4096  /* max DMA buffer per request */

struct io_request {
    uint64_t request_id;       /* opaque — pass back to complete() */
    uint32_t opcode;           /* driver-specific operation code   */
    uint32_t flags;            /* IORQF_* flags                    */
    uint64_t arg[IO_REQ_MAX_ARG]; /* driver-specific arguments     */
    uint64_t buf_phys;         /* physical address of data buffer  */
    uint64_t buf_size;         /* size in bytes                    */
};

/* Flag bits */
#define IORQF_READ   0x0001   /* data flows: device → buf_phys   */
#define IORQF_WRITE  0x0002   /* data flows: buf_phys → device   */

/* ---- Channel handle (opaque, used by kernel and syscalls) ---- */

#define IO_CHANNEL_MAX_NAME 48
#define IO_CHANNEL_MAX      8
#define IO_CHANNEL_QUEUE_DEPTH 16

struct io_channel_entry {
    struct io_request  req;
    volatile int       completed;   /* written by completer, read by waiter */
    int                result;      /* completion status */
    uint32_t           waiting_tid; /* TID of blocked thread, 0 if none */
};

typedef struct {
    char                       name[IO_CHANNEL_MAX_NAME];
    struct io_channel_entry    queue[IO_CHANNEL_QUEUE_DEPTH];
    int                        head;   /* dequeue index (userspace reads) */
    int                        tail;   /* enqueue index (kernel writes)  */
    int                        count;
    uint64_t                   next_seq;   /* request-id source: NEVER a kernel address */
    int                        owner_pid;  /* pid that registered the channel; 0 = kernel */
    spinlock_t                 lock;
} io_channel_t;

/* ---- Kernel API ---- */

/* Create an I/O channel.  Returns handle (0 .. IO_CHANNEL_MAX-1)
 * on success, -1 on failure. */
int io_channel_create(const char *name);

/* Record / test channel ownership.  Channels created by sys_io_register
 * belong to the registering process; every syscall that drives a
 * channel must verify ownership first, or any process could steal and
 * complete another driver's requests.  owner_pid == 0 is the kernel
 * itself (always allowed).  set_owner succeeds only on an unowned
 * (freshly created) channel — a duplicate name must never transfer an
 * existing channel to a new "owner".  Returns 0 / -1. */
int  io_channel_set_owner(int handle, int pid);
int  io_channel_owner_ok(int handle, int pid);

/* Send a request through a channel.
 * Blocks (scheduler-aware) until the userspace driver completes it.
 * Returns 0 on success, negative on error. */
int io_channel_request(int handle, struct io_request *req);

/* Kernel-side lookup by name (used by block_ipc etc.). */
int io_channel_lookup(const char *name);

/* Capability check for phys_map: true if [phys, phys+size) is the data
 * buffer of a pending (not yet completed) request queued on a channel
 * owned by pid.  Lets a userspace driver map its request's bounce page
 * without granting arbitrary-RAM phys_map. */
int io_channel_buf_owned(int pid, uint64_t phys, uint64_t size);

/* Number of channels owned by pid.  Drivers are identified by owning
 * either a device session or an I/O channel; used to gate driver-only
 * resources like contiguous DMA allocations. */
int io_channel_owned_by(int pid);

/* Internal helpers (called from sys_driver.c dispatch handlers).
 * get_request writes through user_req with copy_to_user; the caller
 * must have validated nothing — the copy validates the pointer. */
int io_channel_get_request(int handle, struct io_request *user_req);
int io_channel_complete(int handle, uint64_t request_id, int result);

/* Init */
void io_channel_init(void);

#endif /* IO_CHANNEL_H */
