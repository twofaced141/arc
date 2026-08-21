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


#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "isr.h"
#include "thread.h"
#include "spinlock.h"

#define BITMAP_SIZE  5       /* 140 bits = 5 * 32 */
#define PRIO_ARRAY_BITS 140  /* 0-139 priority levels */

/* O(1) priority array — one queue and one bitmap word per 32 levels */
struct prio_array {
    int nr_active;
    thread_t *queue[PRIO_ARRAY_BITS];
    uint32_t bitmap[BITMAP_SIZE];
};

/* Sleeping threads (blocked with a wake-up deadline) park here per-CPU;
 * each CPU's timer tick wakes the ones whose deadline has passed. */
#define SLEEP_MAX 256

struct runqueue {
    spinlock_t lock;
    unsigned cpu_id;
    prio_array_t arrays[2];
    prio_array_t *active;
    prio_array_t *expired;
    thread_t *current;
    thread_t *idle;
    thread_t *fpu_owner;
    thread_t *sleep_queue[SLEEP_MAX];
    int sleep_count;
};

void scheduler_init(void);
void scheduler_add_thread(thread_t *thread);
void scheduler_remove_thread(thread_t *thread);
void scheduler_unblock_thread(thread_t *thread);
void *scheduler_switch(registers_t *r);
thread_t *scheduler_current_thread(void);

/* Set nice value (-20 .. +19) for a thread, updates static_prio */
void scheduler_set_nice(thread_t *t, int nice);

/* Block the calling thread until clockevent ticks reach deadline.
 * Returns 0 when woken, -1 if the sleep was not set up.
 */
int scheduler_sleep_ticks(uint64_t deadline);
void scheduler_block_current(void);

/* Migrate a thread to a target CPU's runqueue.
 * Returns 0 on success, -1 on failure.
 */
int thread_migrate(thread_t *thread, unsigned target_cpu);

/* Dump per-CPU runqueue load (for load-balancing verification). */
void sched_dump_stats(void);

/* Yield the CPU from kernel-thread context (defined in interrupts.s) */
void thread_yield(void);

#endif
