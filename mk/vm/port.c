#include "port.h"
#include "task.h"
#include "thread.h"
#include "scheduler.h"
#include "vmm.h"
#include "debug.h"
#include "panic.h"
#include "string.h"

#if defined(__x86_64__) || defined(__i386__)
#include "clockevent.h"
#endif

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
#define PORT_ASSERT_VALID(p)   do { if ((p)->magic != PORT_MAGIC) panic_simple("corrupted port"); } while(0)

/* ---- Port management ---- */

/* Ports are individually kmalloc'd, no global static pool.
 * The cspace slot holds a direct pointer (as object_id). */

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

    /* Copy existing messages into the new buffer (linearise the ring) */
    uint32_t count = port->queue_count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t src = (port->queue_head + i) % port->queue_size;
        new_q[i] = port->queue[src];
    }

    kfree(port->queue);
    port->queue = new_q;
    port->queue_size = new_size;
    port->queue_head = 0;
    port->queue_tail = count;
    return 0;
}

/* ---- Internal: move dispose rights into receiver's C-space ------ */
static int deliver_dispose(task_t *receiver, const ipc_msg_t *msg) {
    int moved = 0;
    for (uint32_t i = 0; i < msg->ndispose; i++) {
        uint32_t src_tid = handle_task_id(msg->dispose[i].handle);
        int src_slot = handle_slot(msg->dispose[i].handle);

        task_t *sender = task_find(src_tid);
        if (!sender) continue;

        cslot_t *src = cspace_lookup(&sender->cspace, src_slot);
        if (!src) continue;

        /* Move the slot from sender to receiver */
        int dst_slot = cspace_move(&sender->cspace, &receiver->cspace, src_slot);
        if (dst_slot >= 0) moved++;
    }
    return moved;
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

    p->magic = PORT_MAGIC;
    p->ref_count = 1;
    p->lock = SPINLOCK_INIT;
    p->queue_size = PORT_MIN_QUEUE_SIZE;
    p->queue_head = 0;
    p->queue_tail = 0;
    p->queue_count = 0;
    p->pending_notify = 0;
    p->blocked_tid = 0;

    return p;
}

int port_destroy(ipc_port_t *port) {
    if (!port) return -1;
    PORT_ASSERT_VALID(port);

    uint32_t flags;
    spin_lock_irqsave(&port->lock, &flags);

    port->ref_count = 0;  /* mark dead */

    /* Wake any blocked receiver */
    if (port->blocked_tid) {
        thread_t *waiter = thread_find(port->blocked_tid);
        if (waiter && waiter->state == THREAD_BLOCKED) {
            waiter->state = THREAD_READY;
        }
        port->blocked_tid = 0;
    }

    spin_unlock_irqrestore(&port->lock, flags);

    kfree(port->queue);
    port->queue = NULL;
    kfree(port);
    return 0;
}

/* ---- Send ---- */

int port_send(ipc_port_t *port, const ipc_msg_t *msg, task_t *sender) {
    (void)sender;
    if (!port || !msg) return IPC_ERR_NOSLOT;
    PORT_ASSERT_VALID(port);

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

    port->queue_tail = (port->queue_tail + 1) % port->queue_size;
    port->queue_count++;

    /* Wake blocked receiver */
    if (port->blocked_tid) {
        thread_t *waiter = thread_find(port->blocked_tid);
        if (waiter) {
            scheduler_unblock_thread(waiter);
        }
        port->blocked_tid = 0;
    }

    spin_unlock_irqrestore(&port->lock, flags);

    return IPC_OK;
}

/* ---- Recv ---- */

int port_recv(ipc_port_t *port, ipc_msg_t *msg) {
    if (!port || !msg) return IPC_ERR_NOSLOT;
    PORT_ASSERT_VALID(port);

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
        if (cur) {
            port->blocked_tid = cur->tid;
            scheduler_remove_thread(cur);
            cur->state = THREAD_BLOCKED;
        }
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

    port->blocked_tid = 0;

    /* Dequeue message */
    ipc_msg_t *slot = &port->queue[port->queue_head];
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

    /* Move dispose rights into receiver's C-space */
    task_t *receiver = task_current();
    if (receiver && msg->ndispose > 0) {
        deliver_dispose(receiver, msg);
    }

    return IPC_OK;
}

/* ---- Notify ---- */

int port_notify(ipc_port_t *port) {
    if (!port) return IPC_ERR_NOSLOT;
    PORT_ASSERT_VALID(port);

    uint32_t flags;
    spin_lock_irqsave(&port->lock, &flags);

    port->pending_notify = 1;

    /* Wake blocked receiver */
    if (port->blocked_tid) {
        thread_t *waiter = thread_find(port->blocked_tid);
        if (waiter) {
            scheduler_unblock_thread(waiter);
        }
        port->blocked_tid = 0;
    }

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
    cslot_t *cs = cspace_lookup(&cur->cspace, slot);
    if (!cs || cs->type != CAP_PORT) return IPC_ERR_NOSLOT;

    ipc_port_t *port = (ipc_port_t *)(uintptr_t)cs->object_id;
    port_destroy(port);
    cspace_free_slot(&cur->cspace, slot);
    return IPC_OK;
}

/* ---- Internal: send via handle, msg is already kernel-space ---- */
static int sys_port_send_handle(uint64_t handle, const ipc_msg_t *kmsg) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t *cs = cspace_lookup(&cur->cspace, slot);
    if (!cs || cs->type != CAP_PORT) return IPC_ERR_NOSLOT;
    if (!(cs->rights & CAP_SEND)) return IPC_ERR_NOSLOT;

    ipc_port_t *port = (ipc_port_t *)(uintptr_t)cs->object_id;
    if (!port) return IPC_ERR_NOSLOT;

    return port_send(port, kmsg, cur);
}

int sys_port_send(uint64_t handle, const ipc_msg_t *user_msg) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t *cs = cspace_lookup(&cur->cspace, slot);
    if (!cs || cs->type != CAP_PORT) return IPC_ERR_NOSLOT;
    if (!(cs->rights & CAP_SEND)) return IPC_ERR_NOSLOT;

    ipc_port_t *port = (ipc_port_t *)(uintptr_t)cs->object_id;
    if (!port) return IPC_ERR_NOSLOT;

    ipc_msg_t kmsg;
    if (copy_from_user(&kmsg, user_msg, sizeof(ipc_msg_t)) < 0)
        return IPC_ERR_FAULT;

    return port_send(port, &kmsg, cur);
}

int sys_port_recv(uint64_t handle, ipc_msg_t *user_msg) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;

    int slot = handle_slot(handle);
    cslot_t *cs = cspace_lookup(&cur->cspace, slot);
    if (!cs || cs->type != CAP_PORT) return IPC_ERR_NOSLOT;
    if (!(cs->rights & CAP_RECV)) return IPC_ERR_NOSLOT;

    ipc_port_t *port = (ipc_port_t *)(uintptr_t)cs->object_id;
    if (!port) return IPC_ERR_NOSLOT;

    ipc_msg_t kmsg;
    int ret = port_recv(port, &kmsg);
    if (ret == IPC_OK) {
        if (copy_to_user(user_msg, &kmsg, sizeof(ipc_msg_t)) < 0)
            return IPC_ERR_FAULT;
    }
    return ret;
}

int sys_port_call(uint64_t handle, ipc_msg_t *user_msg) {
    task_t *cur = task_current();
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
    uint64_t reply_handle = task_make_handle(cur->task_id, reply_slot);

    /* Send to target port (non-blocking) — use kernel-to-port helper
     * because kmsg is already on the kernel stack, not user memory. */
    int ret = sys_port_send_handle(handle, &kmsg);
    if (ret != IPC_OK) {
        cspace_free_slot(&cur->cspace, reply_slot);
        return ret;
    }

    /* Block on reply (use existing recv on the call port + embedded reply_handle) */
    ret = sys_port_recv(reply_handle, user_msg);
    cspace_free_slot(&cur->cspace, reply_slot);
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
    cslot_t *cs = cspace_lookup(&cur->cspace, slot);
    if (!cs || cs->type != CAP_PORT) return IPC_ERR_NOSLOT;
    if (!(cs->rights & CAP_SEND)) return IPC_ERR_NOSLOT;

    ipc_port_t *port = (ipc_port_t *)(uintptr_t)cs->object_id;
    return port_notify(port);
}

int sys_port_poll(const uint64_t *user_handles, int count, uint64_t timeout_us) {
    task_t *cur = task_current();
    if (!cur) return IPC_ERR_NOSLOT;
    if (count <= 0 || count > 64) return IPC_ERR_INVAL;

    uint64_t handles[64];
    if (copy_from_user(handles, user_handles, count * sizeof(uint64_t)) < 0)
        return IPC_ERR_FAULT;

    uint64_t deadline = 0;
#if defined(__x86_64__) || defined(__i386__)
    {
        /* timeout_us in microseconds; use only lower 32 bits to avoid
         * __udivdi3 on i386.  A 32-bit microsecond timeout is ~4295
         * seconds, more than enough for any poll. */
        uint32_t ms = (uint32_t)timeout_us / 1000u;
        if (ms > 0)
            deadline = clockevent_get_ticks() + ms;
    }
#endif

    for (;;) {
        for (int i = 0; i < count; i++) {
            int slot = handle_slot(handles[i]);
            cslot_t *cs = cspace_lookup(&cur->cspace, slot);
            if (!cs || cs->type != CAP_PORT) continue;
            if (!(cs->rights & CAP_RECV)) continue;

            ipc_port_t *port = (ipc_port_t *)(uintptr_t)cs->object_id;
            if (!port) continue;

            /* Check if port has data or pending notify */
            if (port->queue_count > 0 || port->pending_notify) {
                return i;  /* return index of ready port */
            }
        }

#if defined(__x86_64__) || defined(__i386__)
        if (timeout_us > 0) {
            if (clockevent_get_ticks() >= (uint32_t)deadline) {
                return IPC_ERR_TIMEOUT;
            }
        }
#endif

        if (timeout_us == 0) {
            return IPC_ERR_EMPTY;  /* non-blocking poll */
        }

        /* Blocking poll: mark current thread blocked */
        {
            thread_t *cur_t = thread_current();
            if (cur_t) {
                cur_t->state = THREAD_BLOCKED;
            }
        }

        return IPC_ERR_EMPTY;  /* caller must retry */
    }
}
