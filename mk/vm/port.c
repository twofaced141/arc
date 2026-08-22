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


#include "port.h"
#include "task.h"
#include "thread.h"
#include "scheduler.h"
#include "vmm.h"
#include "debug.h"
#include "panic.h"
#include "string.h"
#include "clockevent.h"

/* ================================================================
 * port.c — Capability-based IPC ports
 *
 * Ports are heap-allocated (kmalloc).  Each port's message queue is
 * a dynamically allocated circular buffer that grows by 2× when full
 * up to PORT_MAX_QUEUE_SIZE.
 * ================================================================ */

#define PORT_MIN_QUEUE_SIZE    16   /* initial queue slots */
#define PORT_MAX_QUEUE_SIZE    2048 /* cap per port */

#define PORT_MAGIC             0x504F5254   /* "PORT" */
/* A dead port (destroyed, or never valid) must be rejected GRACEFULLY
 * by send/recv/notify — the old PORT_ASSERT_VALID paniced the kernel
 * on a stale capability, giving any task a one-syscall panic. */
#define PORT_IS_VALID(p)       ((p) && (p)->magic == PORT_MAGIC)

/* ---- Global port table ----
 *
 * Ports are individually kmalloc'd and referenced by raw pointers
 * stored in cspace slots.  Without lifetime management, every
 * capability-to-pointer resolution was a TOCTOU use-after-free: a
 * sibling thread could destroy the port between lookup and use, and
 * a slot whose port died elsewhere kept a dangling object_id forever.
 *
 * The table is the single source of liveness: a pointer is only ever
 * dereferenced as a port AFTER it has been found in this table and a
 * reference taken (both under port_table_lock).  A freed port is
 * unregistered first, so stale capabilities fail gracefully with
 * IPC_ERR_NOSLOT instead of touching freed memory. */
#define PORT_TABLE_MAX         512

static ipc_port_t *port_table[PORT_TABLE_MAX];
static spinlock_t port_table_lock = SPINLOCK_INIT;

/* Take a reference on the raw object pointer stored in a cspace slot.
 * Returns NULL for dead or unknown ports.  The caller must balance
 * with port_release(). */
static ipc_port_t *port_ref_raw(void *raw) {
    ipc_port_t *p = (ipc_port_t *)raw;
    if (!p)
        return NULL;

    uint32_t flags;
    spin_lock_irqsave(&port_table_lock, &flags);
    for (int i = 0; i < PORT_TABLE_MAX; i++) {
        if (port_table[i] != p)
            continue;
        if (p->magic != PORT_MAGIC)
            break;              /* registered but already dead */
        p->ref_count++;
        spin_unlock_irqrestore(&port_table_lock, flags);
        return p;
    }
    spin_unlock_irqrestore(&port_table_lock, flags);
    return NULL;
}

void port_release(ipc_port_t *port) {
    if (!port)
        return;

    uint32_t flags;
    int dead = 0;
    spin_lock_irqsave(&port_table_lock, &flags);
    /* Membership check makes this safe against stale pointers. */
    for (int i = 0; i < PORT_TABLE_MAX; i++) {
        if (port_table[i] != port)
            continue;
        if (--port->ref_count <= 0) {
            port_table[i] = NULL;
            dead = 1;
        }
        break;
    }
    spin_unlock_irqrestore(&port_table_lock, flags);

    if (!dead)
        return;
    kfree(port->queue);
    kfree(port->queue_senders);
    port->queue = NULL;
    port->queue_senders = NULL;
    kfree(port);
}

/* ---- Internal: grow the message queue (double capacity) ---- */
static int port_grow_queue(ipc_port_t *port) {
    uint32_t new_size = port->queue_size < PORT_MIN_QUEUE_SIZE
                        ? PORT_MIN_QUEUE_SIZE
                        : port->queue_size * 2;
    if (new_size > PORT_MAX_QUEUE_SIZE)
        return -1;

    ipc_msg_t *new_q = (ipc_msg_t *)kmalloc(new_size * sizeof(ipc_msg_t));
    if (!new_q)
        return -1;

    uint32_t *new_senders = (uint32_t *)kmalloc(new_size * sizeof(uint32_t));
    if (!new_senders) {
        kfree(new_q);
        return -1;
    }

    /* Copy existing messages into the new buffer (linearise the ring) */
    uint32_t count = port->queue_count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t src = (port->queue_head + i) % port->queue_size;
        new_q[i] = port->queue[src];
        new_senders[i] = port->queue_senders[src];
    }

    kfree(port->queue);
    kfree(port->queue_senders);
    port->queue = new_q;
    port->queue_senders = new_senders;
    port->queue_size = new_size;
    port->queue_head = 0;
    port->queue_tail = count;
    return 0;
}

/* ---- Internal: validate dispose descriptors against the sender's C-space.
 *
 * A sender may only dispose of capabilities it actually holds: every
 * dispose handle must name a slot in the *sender's* cspace whose type
 * matches the descriptor.  Without this check a malicious process could
 * forge handles naming another task's slots, and deliver_dispose()
 * (which resolves the source cspace from the handle's task id) would
 * move that task's capabilities into the receiver's cspace. ---- */
static int validate_dispose(task_t *sender, const ipc_msg_t *msg) {
    if (!msg->ndispose)
        return 0;
    if (!sender)
        return -1;
    if (msg->ndispose > IPC_DISPOSE_MAX)
        return -1;

    for (uint32_t i = 0; i < msg->ndispose; i++) {
        if (handle_task_id(msg->dispose[i].handle) != sender->task_id)
            return -1;
        int slot = handle_slot(msg->dispose[i].handle);
        cslot_t snap;
        if (cspace_lookup_snapshot(&sender->cspace, slot, &snap) < 0)
            return -1;
        if (!snap.in_use)
            return -1;
        if (snap.type != msg->dispose[i].type)
            return -1;
    }
    return 0;
}

/* ---- Internal: move dispose rights into receiver's C-space ----
 *
 * The source cspace is resolved from the *recorded* sender task_id
 * (stored in the queue slot at send time), never from the handle's
 * task id field — a sender can forge that field to name a foreign
 * task's slots. */
static int deliver_dispose(task_t *receiver, const ipc_msg_t *msg,
                           uint32_t sender_tid) {
    if (sender_tid == 0)
        return 0;

    /* Hold the task lock across the whole operation: plain task_find
     * releases it before returning, so a concurrent task_destroy could
     * kfree the sender (and its cspace) between lookup and use. */
    uint32_t task_flags;
    task_t *sender = task_find_hold(sender_tid, &task_flags);
    if (!sender)
        return 0;   /* sender died — nothing to move */

    int moved = 0;
    for (uint32_t i = 0; i < msg->ndispose; i++) {
        int src_slot = handle_slot(msg->dispose[i].handle);

        /* Belt and braces: the handle must still name the recorded
         * sender, not some other task. */
        if (handle_task_id(msg->dispose[i].handle) != sender_tid)
            continue;

        cslot_t src_snap;
        if (cspace_lookup_snapshot(&sender->cspace, src_slot, &src_snap) < 0)
            continue;
        if (src_snap.type != msg->dispose[i].type)
            continue;

        /* Move the slot from sender to receiver */
        int dst_slot = cspace_move(&sender->cspace, &receiver->cspace, src_slot);
        if (dst_slot >= 0) moved++;
    }
    task_put(sender, task_flags);
    return moved;
}

/* ---- Waiter list (under port->lock) ---- */

static void port_wait_register_locked(ipc_port_t *port, uint32_t tid) {
    for (int i = 0; i < port->waiter_count; i++)
        if (port->waiters[i] == tid)
            return;                     /* already registered */
    if (port->waiter_count >= PORT_WAITERS_MAX)
        return;                         /* list full: caller falls back
                                         * to its timeout / retry path */
    port->waiters[port->waiter_count++] = tid;
}

static void port_wake_all_locked(ipc_port_t *port) {
    for (int i = 0; i < port->waiter_count; i++) {
        thread_t *w = thread_find(port->waiters[i]);
        if (w)
            scheduler_unblock_thread(w);
    }
    port->waiter_count = 0;
}

/* ---- Port creation / destruction ---- */

ipc_port_t *port_create(void) {
    ipc_port_t *p = (ipc_port_t *)kmalloc(sizeof(ipc_port_t));
    if (!p)
        return NULL;

    p->queue = (ipc_msg_t *)kmalloc(PORT_MIN_QUEUE_SIZE * sizeof(ipc_msg_t));
    if (!p->queue) {
        kfree(p);
        return NULL;
    }

    p->queue_senders = (uint32_t *)kmalloc(PORT_MIN_QUEUE_SIZE * sizeof(uint32_t));
    if (!p->queue_senders) {
        kfree(p->queue);
        kfree(p);
        return NULL;
    }

    /* Reserve a table slot before publishing the port. */
    uint32_t tflags;
    spin_lock_irqsave(&port_table_lock, &tflags);
    int table_slot = -1;
    for (int i = 0; i < PORT_TABLE_MAX; i++) {
        if (!port_table[i]) {
            table_slot = i;
            break;
        }
    }
    if (table_slot < 0) {
        spin_unlock_irqrestore(&port_table_lock, tflags);
        kfree(p->queue_senders);
        kfree(p->queue);
        kfree(p);
        return NULL;
    }

    p->magic = PORT_MAGIC;
    p->ref_count = 1;             /* the creator's cspace slot */
    p->lock = SPINLOCK_INIT;
    p->queue_size = PORT_MIN_QUEUE_SIZE;
    p->queue_head = 0;
    p->queue_tail = 0;
    p->queue_count = 0;
    p->pending_notify = 0;
    p->waiter_count = 0;

    port_table[table_slot] = p;
    spin_unlock_irqrestore(&port_table_lock, tflags);

    return p;
}

int port_destroy(ipc_port_t *port) {
    if (!PORT_IS_VALID(port))
        return -1;

    /* Wake any blocked receiver first.  The thread was pulled off the
     * runqueue by port_recv/poll (scheduler_remove_thread), so it must
     * be re-enqueued — a raw state write would strand it forever. */
    uint32_t flags;
    spin_lock_irqsave(&port->lock, &flags);
    port_wake_all_locked(port);
    spin_unlock_irqrestore(&port->lock, flags);

    /* Kill + drop the creator's reference under the table lock so no
     * concurrent port_ref_raw() can slip a reference past the death
     * mark: after magic is cleared here, lookups fail cleanly.
     * (Inlined release logic — port_release() takes this same lock.) */
    uint32_t tflags;
    int dead = 0;
    spin_lock_irqsave(&port_table_lock, &tflags);
    port->magic = 0;      /* mark dead FIRST — no new sends can pass */
    for (int i = 0; i < PORT_TABLE_MAX; i++) {
        if (port_table[i] != port)
            continue;
        if (--port->ref_count <= 0) {
            port_table[i] = NULL;
            dead = 1;
        }
        break;
    }
    spin_unlock_irqrestore(&port_table_lock, tflags);

    if (!dead)
        return 0;
    kfree(port->queue);
    kfree(port->queue_senders);
    port->queue = NULL;
    port->queue_senders = NULL;
    kfree(port);
    return 0;
}

/* ---- Send ---- */

int port_send(ipc_port_t *port, const ipc_msg_t *msg, task_t *sender) {
    if (!PORT_IS_VALID(port) || !msg) return IPC_ERR_NOSLOT;

    /* Capability-transfer sanity: the sender may only dispose of
     * capabilities it holds (see validate_dispose). */
    if (validate_dispose(sender, msg) < 0)
        return IPC_ERR_INVAL;

    uint32_t flags;
    spin_lock_irqsave(&port->lock, &flags);

    /* Grow queue if full */
    if (port->queue_count >= port->queue_size) {
        if (port_grow_queue(port) < 0) {
            spin_unlock_irqrestore(&port->lock, flags);
            return IPC_ERR_FULL;
        }
    }

    /* Copy message into queue */
    ipc_msg_t *slot = &port->queue[port->queue_tail];
    slot->msg_id     = msg->msg_id;
    slot->data_size  = msg->data_size;
    slot->ndispose   = msg->ndispose;

    if (msg->data_size > IPC_DATA_SIZE)
        slot->data_size = IPC_DATA_SIZE;
    memcpy(slot->data, msg->data, slot->data_size);

    /* Copy dispose descriptors (rights are moved below) */
    uint32_t ndispose = msg->ndispose;
    if (ndispose > IPC_DISPOSE_MAX) ndispose = IPC_DISPOSE_MAX;
    slot->ndispose = ndispose;
    for (uint32_t i = 0; i < ndispose; i++) {
        slot->dispose[i] = msg->dispose[i];
    }

    /* Record the true sender: deliver_dispose() resolves the source
     * cspace from this value, never from the handle in the message. */
    port->queue_senders[port->queue_tail] = sender ? sender->task_id : 0;

    port->queue_tail = (port->queue_tail + 1) % port->queue_size;
    port->queue_count++;

    /* Wake ALL blocked receivers: with a single blocked_tid, every
     * receiver but the last registrant was stranded forever. */
    port_wake_all_locked(port);

    spin_unlock_irqrestore(&port->lock, flags);

    return IPC_OK;
}

/* ---- Recv ---- */

int port_recv(ipc_port_t *port, ipc_msg_t *msg) {
    if (!PORT_IS_VALID(port) || !msg) return IPC_ERR_NOSLOT;

    uint32_t flags;
    spin_lock_irqsave(&port->lock, &flags);

    /* Check notify before blocking */
    if (port->queue_count == 0 && port->pending_notify) {
        port->pending_notify = 0;
        msg->msg_id    = 0;
        msg->data_size = 0;
        msg->ndispose  = 0;
        spin_unlock_irqrestore(&port->lock, flags);
        return IPC_OK;
    }

    /* Block until message arrives */
    while (port->queue_count == 0 && !port->pending_notify) {
        thread_t *cur = thread_current();
        if (!cur) {
            spin_unlock_irqrestore(&port->lock, flags);
            return IPC_ERR_EMPTY;   /* no thread context: caller retries */
        }
        /* Register as waiter and park — both under the port lock, so a
         * sender's wake (also under the port lock) can never land in
         * the window between runqueue removal and the BLOCKED state. */
        port_wait_register_locked(port, cur->tid);
        scheduler_remove_thread(cur);
        cur->state = THREAD_BLOCKED;
        spin_unlock_irqrestore(&port->lock, flags);

        /* Return to syscall dispatcher; scheduler_switch will pick another
         * thread because we're BLOCKED.  When port_send/notify wakes us,
         * the retry loop in userspace will call recv again. */
        return IPC_ERR_EMPTY;
    }

    /* Check notify again */
    if (port->queue_count == 0 && port->pending_notify) {
        port->pending_notify = 0;
        msg->msg_id    = 0;
        msg->data_size = 0;
        msg->ndispose  = 0;
        spin_unlock_irqrestore(&port->lock, flags);
        return IPC_OK;
    }

    /* Dequeue message */
    ipc_msg_t *slot = &port->queue[port->queue_head];
    uint32_t sender_tid = port->queue_senders[port->queue_head];
    msg->msg_id    = slot->msg_id;
    msg->data_size = slot->data_size;
    msg->ndispose  = slot->ndispose;
    memcpy(msg->data, slot->data, slot->data_size);
    for (uint32_t i = 0; i < slot->ndispose; i++) {
        msg->dispose[i] = slot->dispose[i];
    }

    port->queue_head = (port->queue_head + 1) % port->queue_size;
    port->queue_count--;

    spin_unlock_irqrestore(&port->lock, flags);

    /* Move dispose rights into receiver's C-space.  The source is the
     * recorded sender, not the handle's (forgable) task id. */
    task_t *receiver = task_current();
    if (receiver && msg->ndispose > 0) {
        deliver_dispose(receiver, msg, sender_tid);
    }

    return IPC_OK;
}

/* ---- Notify ---- */

int port_notify(ipc_port_t *port) {
    if (!PORT_IS_VALID(port)) return IPC_ERR_NOSLOT;

    uint32_t flags;
    spin_lock_irqsave(&port->lock, &flags);

    port->pending_notify = 1;

    /* Wake ALL blocked receivers */
    port_wake_all_locked(port);

    spin_unlock_irqrestore(&port->lock, flags);
    return IPC_OK;
}

/* ================================================================
 * Syscall handlers
 * ================================================================ */

/*
 * Syscall calling convention (amd64):
 *   rax = syscall number
 *   rdi = arg1, rsi = arg2, rdx = arg3, r10 = arg4
 *
 * Syscall numbers for Phase 1:
 *   0-12   — existing (thread, IPC legacy)
 *   13-19  — new task/cspace/port syscalls
 *   1024+  — BSD layer
 */

/* ---- Internal: resolve a capability slot to a referenced port ----
 *
 * Returns the port with one reference taken (caller must
 * port_release()), or NULL if the slot is not a usable port
 * capability.  The reference closes the destroy-vs-use window: the
 * port memory stays alive until we are done with it. */
static ipc_port_t *port_from_cslot(cslot_t *cs, uint32_t need_rights) {
    if (!cs || cs->type != CAP_PORT)
        return NULL;
    if (!(cs->rights & need_rights))
        return NULL;
    return port_ref_raw((void *)(uintptr_t)cs->object_id);
}

int sys_port_create(void) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    ipc_port_t *port = port_create();
    if (!port) return IPC_ERR_NOSLOT;

    /* Allocate a slot in current task's C-space with SEND|RECV rights */
    int slot = cspace_alloc_slot(&cur->cspace, CAP_PORT,
                                  CAP_SEND | CAP_RECV | CAP_REPLY,
                                  (uint64_t)(uintptr_t)port);
    if (slot < 0) {
        port_destroy(port);
        return IPC_ERR_NOSLOT;
    }

    return (int)task_make_handle(cur->task_id, slot);
}

int sys_port_destroy(uint64_t handle) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t snap;
    if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
        return IPC_ERR_NOSLOT;

    /* Resolve through the table so a stale/dangling object_id fails
     * gracefully instead of dereferencing freed memory.  Destroy only
     * needs to mark death and drop OUR lookup reference: freeing the
     * cspace slot below releases the slot's own reference, which is
     * the last one for an undestroyed port. */
    ipc_port_t *port = port_ref_raw((void *)(uintptr_t)snap.object_id);
    if (!port) return IPC_ERR_NOSLOT;

    port_destroy(port);
    port_release(port);          /* drop the lookup reference */
    cspace_free_slot(&cur->cspace, slot);
    return IPC_OK;
}

/* ---- Internal: send via handle, msg is already kernel-space ---- */
static int sys_port_send_handle(uint64_t handle, const ipc_msg_t *kmsg) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t snap;
    if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
        return IPC_ERR_NOSLOT;

    ipc_port_t *port = port_from_cslot(&snap, CAP_SEND);
    if (!port) return IPC_ERR_NOSLOT;

    int ret = port_send(port, kmsg, cur);
    port_release(port);
    return ret;
}

int sys_port_send(uint64_t handle, const ipc_msg_t *user_msg) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t snap;
    if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
        return IPC_ERR_NOSLOT;

    ipc_port_t *port = port_from_cslot(&snap, CAP_SEND);
    if (!port) return IPC_ERR_NOSLOT;

    ipc_msg_t kmsg;
    int ret = copy_from_user(&kmsg, user_msg, sizeof(ipc_msg_t)) < 0
              ? IPC_ERR_FAULT
              : port_send(port, &kmsg, cur);

    port_release(port);
    return ret;
}

int sys_port_recv(uint64_t handle, ipc_msg_t *user_msg) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t snap;
    if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
        return IPC_ERR_NOSLOT;

    ipc_port_t *port = port_from_cslot(&snap, CAP_RECV);
    if (!port) return IPC_ERR_NOSLOT;

    ipc_msg_t kmsg;
    int ret = port_recv(port, &kmsg);
    if (ret == IPC_OK) {
        if (copy_to_user(user_msg, &kmsg, sizeof(ipc_msg_t)) < 0)
            ret = IPC_ERR_FAULT;
    }
    port_release(port);
    return ret;
}

int sys_port_call(uint64_t handle, ipc_msg_t *user_msg) {
    task_t *cur = task_current();
    thread_t *cur_thr = thread_current();
    if (!cur) return IPC_ERR_NOSLOT;

    /* Read message from user-space */
    ipc_msg_t kmsg;
    if (copy_from_user(&kmsg, user_msg, sizeof(ipc_msg_t)) < 0)
        return IPC_ERR_FAULT;

    /* Create a temporary reply port */
    ipc_port_t *reply_port = port_create();
    if (!reply_port) return IPC_ERR_NOSLOT;

    /* Allocate a slot for the reply port (RECV only) */
    int reply_slot = cspace_alloc_slot(&cur->cspace, CAP_PORT, CAP_RECV | CAP_REPLY,
                                        (uint64_t)(uintptr_t)reply_port);
    if (reply_slot < 0) {
        port_destroy(reply_port);
        return IPC_ERR_NOSLOT;
    }

    /* Send to target port (non-blocking) — use kernel-to-port helper
     * because kmsg is already on the kernel stack, not user memory. */
    int ret = sys_port_send_handle(handle, &kmsg);
    if (ret != IPC_OK) {
        port_destroy(reply_port);                    /* mark dead + drop ref */
        cspace_free_slot(&cur->cspace, reply_slot);  /* slot ref (no-op now) */
        return ret;
    }

    /* Wait for the reply IN KERNEL.  The old code did a single recv,
     * which returns IPC_ERR_EMPTY when nothing is queued yet, and then
     * destroyed the reply port out from under the server.  Park like
     * port_recv does (register + remove + BLOCKED) and retry until the
     * server's reply wakes us or the port dies. */
    ipc_msg_t reply;
    for (;;) {
        ret = port_recv(reply_port, &reply);
        if (ret != IPC_ERR_EMPTY || !cur_thr)
            break;

        uint32_t flags;
        spin_lock_irqsave(&reply_port->lock, &flags);
        if (reply_port->magic != PORT_MAGIC) {
            spin_unlock_irqrestore(&reply_port->lock, flags);
            ret = IPC_ERR_NOSLOT;      /* destroyed meanwhile */
            break;
        }
        if (reply_port->queue_count > 0 || reply_port->pending_notify) {
            /* Reply raced in before we parked — retry recv at once. */
            spin_unlock_irqrestore(&reply_port->lock, flags);
            continue;
        }
        port_wait_register_locked(reply_port, cur_thr->tid);
        scheduler_remove_thread(cur_thr);
        cur_thr->state = THREAD_BLOCKED;
        spin_unlock_irqrestore(&reply_port->lock, flags);

        thread_yield();   /* resume when a reply/notify/destroy wakes us */
    }

    if (ret == IPC_OK) {
        if (copy_to_user(user_msg, &reply, sizeof(ipc_msg_t)) < 0)
            ret = IPC_ERR_FAULT;
    }

    port_destroy(reply_port);                    /* mark dead + drop ref */
    cspace_free_slot(&cur->cspace, reply_slot);  /* slot ref (no-op now) */
    return ret;
}

int sys_port_reply(uint64_t reply_handle, const ipc_msg_t *user_msg) {
    /* Reply is just a send to the reply port handle */
    return sys_port_send(reply_handle, user_msg);
}

int sys_port_notify(uint64_t handle) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t snap;
    if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
        return IPC_ERR_NOSLOT;

    ipc_port_t *port = port_from_cslot(&snap, CAP_SEND);
    if (!port) return IPC_ERR_NOSLOT;

    int ret = port_notify(port);
    port_release(port);
    return ret;
}

int sys_port_poll(const uint64_t *user_handles, int count, uint64_t timeout_us) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;
    if (count <= 0 || count > 64) return IPC_ERR_INVAL;

    uint64_t handles[64];
    if (copy_from_user(handles, user_handles, count * sizeof(uint64_t)) < 0)
        return IPC_ERR_FAULT;

    uint64_t deadline = 0;
    /* timeout_us in microseconds; use only lower 32 bits so i386 needs
     * no __udivdi3.  A 32-bit microsecond timeout is ~4295 seconds,
     * more than enough for any poll. */
    {
        uint32_t ms = (uint32_t)timeout_us / 1000u;
        if (ms > 0)
            deadline = clockevent_get_ticks() + ms;
    }

    for (;;) {
        for (int i = 0; i < count; i++) {
            int slot = handle_slot(handles[i]);
            cslot_t snap;
            if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
                continue;

            ipc_port_t *port = port_from_cslot(&snap, CAP_RECV);
            if (!port) continue;

            /* Check if port has data or pending notify (snapshot under
             * the port lock). */
            uint32_t pflags;
            spin_lock_irqsave(&port->lock, &pflags);
            int ready = (port->queue_count > 0 || port->pending_notify);
            spin_unlock_irqrestore(&port->lock, pflags);
            port_release(port);

            if (ready)
                return i;  /* return index of ready port */
        }

        if (timeout_us > 0) {
            if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
                return IPC_ERR_TIMEOUT;
        }

        if (timeout_us == 0) {
            return IPC_ERR_EMPTY;  /* non-blocking poll */
        }

        /* Blocking poll: register as waiter on every polled port so a
         * send/notify/destroy wakes us early, then arm the tick
         * deadline.  The old code only wrote THREAD_BLOCKED and
         * returned: the thread was never dequeued, never re-armed, and
         * could never be woken — it vanished from the scheduler
         * forever along with its kernel stack. */
        thread_t *cur_t = thread_current();
        if (cur_t) {
            for (int i = 0; i < count; i++) {
                int slot = handle_slot(handles[i]);
                cslot_t snap;
                if (cspace_lookup_snapshot(&cur->cspace, slot, &snap) < 0)
                    continue;
                ipc_port_t *port = port_from_cslot(&snap, CAP_RECV);
                if (!port) continue;

                uint32_t pflags;
                spin_lock_irqsave(&port->lock, &pflags);
                if (PORT_IS_VALID(port))
                    port_wait_register_locked(port, cur_t->tid);
                spin_unlock_irqrestore(&port->lock, pflags);
                port_release(port);
            }

            /* Sleep until the deadline; a wake from any registered
             * port resumes us sooner.  sret==0 means the deadline has
             * already passed (the scan above reports TIMEOUT next). */
            int sret = scheduler_sleep_ticks(deadline);
            if (sret != 0)
                thread_yield();
        } else {
            thread_yield();   /* no thread context: avoid a hot spin */
        }
    }
}
