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


#include "io_channel.h"
#include "debug.h"
#include "string.h"
#include "thread.h"
#include "scheduler.h"


static io_channel_t channels[IO_CHANNEL_MAX];
static int channel_count;
static spinlock_t channel_lock = SPINLOCK_INIT;


int io_channel_create(const char *name) {
    uint32_t flags;
    spin_lock_irqsave(&channel_lock, &flags);

    if (channel_count >= IO_CHANNEL_MAX) {
        spin_unlock_irqrestore(&channel_lock, flags);
        log_print(LOG_LEVEL_ERROR, "io_channel: table full\n");
        return -1;
    }

    /* Check for duplicate */
    for (int i = 0; i < channel_count; i++) {
        if (strcmp(channels[i].name, name) == 0) {
            spin_unlock_irqrestore(&channel_lock, flags);
            return i; /* already exists */
        }
    }

    int h = channel_count++;
    io_channel_t *ch = &channels[h];
    memset(ch, 0, sizeof(io_channel_t));

    size_t n = strlen(name);
    if (n >= IO_CHANNEL_MAX_NAME) n = IO_CHANNEL_MAX_NAME - 1;
    memcpy(ch->name, name, n);
    ch->name[n] = '\0';

    ch->head = 0;
    ch->tail = 0;
    ch->count = 0;
    ch->lock = SPINLOCK_INIT;

    spin_unlock_irqrestore(&channel_lock, flags);

    log_printf(LOG_LEVEL_DEBUG, "io_channel: created '%s' handle=%d\n", name, h);
    return h;
}

int io_channel_lookup(const char *name) {
    uint32_t flags;
    spin_lock_irqsave(&channel_lock, &flags);
    for (int i = 0; i < channel_count; i++) {
        if (strcmp(channels[i].name, name) == 0) {
            spin_unlock_irqrestore(&channel_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&channel_lock, flags);
    return -1;
}

int io_channel_request(int handle, struct io_request *req) {
    if (handle < 0 || handle >= channel_count)
        return -1;

    io_channel_t *ch = &channels[handle];
    if (ch->count >= IO_CHANNEL_QUEUE_DEPTH)
        return -1; /* queue full */

    uint32_t flags;
    spin_lock_irqsave(&ch->lock, &flags);

    int slot = ch->tail;
    struct io_channel_entry *entry = &ch->queue[slot];
    memcpy(&entry->req, req, sizeof(*req));
    entry->req.request_id = (uint64_t)(uintptr_t)entry; /* unique ID */
    entry->completed = 0;
    entry->result = 0;

    ch->tail = (ch->tail + 1) % IO_CHANNEL_QUEUE_DEPTH;
    ch->count++;

    /* Store the waiting thread's TID so io_channel_complete can wake us.
     * Block the current thread BEFORE releasing the lock to close the
     * race where the driver completes the request before we block. */
    entry->waiting_tid = thread_get_tid();
    thread_current()->state = THREAD_BLOCKED;

    spin_unlock_irqrestore(&ch->lock, flags);

    /* Yield the CPU until the userspace driver completes the request.
     *
     * On x86: thread_yield() builds a fake interrupt frame and calls
     * scheduler_switch, which sees THREAD_BLOCKED and context-switches
     * away.  When io_channel_complete() unblocks us, scheduler_switch
     * picks us again and returns through .yield_resume.
     *
     * On arm64: enable IRQs and WFI.  The next timer tick triggers
     * scheduler_switch, which sees THREAD_BLOCKED and switches away.
     * When unblocked and rescheduled, we resume after WFI.
     *
     * A spurious wake (entry still not completed) loops back and
     * re-blocks. */
    while (!entry->completed) {
#if defined(__x86_64__) || defined(__i386__)
        thread_yield();
#elif defined(__aarch64__)
        __asm__ __volatile__("msr daifclr, #2" ::: "memory");
        __asm__ __volatile__("wfi");
#endif
    }

    /* Back from unblock — request is complete */
    thread_current()->state = THREAD_RUNNING;

    /* Restore the result pointer for the caller */
    if (req)
        req->request_id = entry->req.request_id;

    return entry->result;
}


int io_channel_get_request(int handle, struct io_request *user_req) {
    if (handle < 0 || handle >= channel_count)
        return -1;

    io_channel_t *ch = &channels[handle];

    uint32_t flags;
    spin_lock_irqsave(&ch->lock, &flags);

    if (ch->count == 0) {
        spin_unlock_irqrestore(&ch->lock, flags);
        return 0; /* no request pending */
    }

    int slot = ch->head;
    struct io_channel_entry *entry = &ch->queue[slot];
    ch->head = (ch->head + 1) % IO_CHANNEL_QUEUE_DEPTH;
    ch->count--;

    spin_unlock_irqrestore(&ch->lock, flags);

    /* Copy request to userspace */
    memcpy(user_req, &entry->req, sizeof(entry->req));
    return 1; /* one request returned */
}

int io_channel_complete(int handle, uint64_t request_id, int result) {
    if (handle < 0 || handle >= channel_count)
        return -1;

    io_channel_t *ch = &channels[handle];

    /* Linear scan to find the request by ID (request_id == entry address).
     * For IO_CHANNEL_QUEUE_DEPTH=16 this is negligible. */
    uint32_t flags;
    spin_lock_irqsave(&ch->lock, &flags);

    for (int i = 0; i < IO_CHANNEL_QUEUE_DEPTH; i++) {
        struct io_channel_entry *entry = &ch->queue[i];
        if (!entry->completed &&
            entry->req.request_id == request_id) {
            entry->result = result;
            entry->completed = 1;

            /* Wake the thread waiting for this request, if any.
             * Read waiting_tid under the lock, then unblock outside. */
            uint32_t wake_tid = entry->waiting_tid;
            entry->waiting_tid = 0;

            spin_unlock_irqrestore(&ch->lock, flags);

            if (wake_tid) {
                thread_t *waiter = thread_find(wake_tid);
                if (waiter)
                    scheduler_unblock_thread(waiter);
            }

            return 0;
        }
    }

    spin_unlock_irqrestore(&ch->lock, flags);
    return -1; /* request not found (already completed or invalid) */
}


void io_channel_init(void) {
    memset(channels, 0, sizeof(channels));
    channel_count = 0;
    channel_lock = SPINLOCK_INIT;
    log_print(LOG_LEVEL_DEBUG, "io_channel: init\n");
}
