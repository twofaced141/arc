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


#include "scheduler.h"
#include "thread.h"
#include "pmm.h"
#include "memory.h"
#include "uart.h"
#include "string.h"
#include "vmm.h"
#include "clockevent.h"

static prio_array_t active_array;
static prio_array_t expired_array;
static prio_array_t *active  = &active_array;
static prio_array_t *expired = &expired_array;
static thread_t    *current_thread;
static thread_t    *idle_thread;
static uint64_t ticks;

/* Sleeping threads (see amd64 scheduler for the design). */
#define SLEEP_MAX 256
static thread_t *sleep_queue[SLEEP_MAX];
static int sleep_count;

static void sleep_wake_tick(void);

static inline int __ffs(uint32_t word) {
    return __builtin_ctz(word);
}

static void prio_array_enqueue(prio_array_t *pa, thread_t *t, int prio) {
    if (pa->queue[prio]) {
        thread_t *head = pa->queue[prio];
        thread_t *tail = head->prev;
        t->next = head;
        t->prev = tail;
        tail->next = t;
        head->prev = t;
    } else {
        pa->queue[prio] = t;
        t->next = t;
        t->prev = t;
        pa->bitmap[prio / 32] |= (1u << (prio % 32));
    }
    t->array = pa;
    pa->nr_active++;
    {
        //uart_print(pa == &active_array ? "enq:A " : "enq:E ");
        //uart_print_hex64(t->tid);
        //uart_print(" p=");
        //uart_print_hex64(prio);
        //uart_print(" n=");
        //uart_print_hex64(pa->nr_active);
        //uart_print("\n");
    }
}

static void prio_array_dequeue(prio_array_t *pa, thread_t *t, int prio) {
    if (t->next == t) {
        pa->queue[prio] = 0;
        pa->bitmap[prio / 32] &= ~(1u << (prio % 32));
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (pa->queue[prio] == t)
            pa->queue[prio] = t->next;
    }
    t->next = 0;
    t->prev = 0;
    t->array = 0;
    pa->nr_active--;
    {
        //uart_print(pa == &active_array ? "deq:A " : "deq:E ");
        //uart_print_hex64(t->tid);
        //uart_print(" p=");
        //uart_print_hex64(prio);
        //uart_print(" n=");
        //uart_print_hex64(pa->nr_active);
        //uart_print("\n");
    }
}

static int prio_array_find_top(const prio_array_t *pa) {
    for (int i = 0; i < 5; i++) {
        if (pa->bitmap[i])
            return i * 32 + __ffs(pa->bitmap[i]);
    }
    return -1;
}

#define DEFAULT_PRIO 120

static inline uint32_t prio_to_timeslice(int static_prio) {
    return (PRIO_MAX - 1 - static_prio) * 2 + 2;
}

static inline int effective_prio(int static_prio, int sleep_avg) {
    int bonus = (sleep_avg * 11) / (MAX_SLEEP_AVG + 1);
    if (bonus > 10) bonus = 10;
    int p = static_prio - bonus + 5;
    if (p < MAX_RT_PRIO) p = MAX_RT_PRIO;
    if (p >= PRIO_MAX)   p = PRIO_MAX - 1;
    return p;
}

/* AArch64 has no x87-style lazy mechanism in EL1 (no CR0.TS), so the  */
/* Q0-Q31/FPSR/FPCR state is saved and restored on every context       */
/* switch.  The kernel never executes FP/SIMD instructions, so the     */
/* restored state survives untouched until eret.                       */

static inline void fpu_save(thread_t *t) {
    uint32_t fpsr, fpcr;
    uint64_t *p = (uint64_t *)t->fpu_state;
    __asm__ __volatile__(
        "stp q0,  q1,  [%0, #0]\n\t"
        "stp q2,  q3,  [%0, #32]\n\t"
        "stp q4,  q5,  [%0, #64]\n\t"
        "stp q6,  q7,  [%0, #96]\n\t"
        "stp q8,  q9,  [%0, #128]\n\t"
        "stp q10, q11, [%0, #160]\n\t"
        "stp q12, q13, [%0, #192]\n\t"
        "stp q14, q15, [%0, #224]\n\t"
        "stp q16, q17, [%0, #256]\n\t"
        "stp q18, q19, [%0, #288]\n\t"
        "stp q20, q21, [%0, #320]\n\t"
        "stp q22, q23, [%0, #352]\n\t"
        "stp q24, q25, [%0, #384]\n\t"
        "stp q26, q27, [%0, #416]\n\t"
        "stp q28, q29, [%0, #448]\n\t"
        "stp q30, q31, [%0, #480]\n\t"
        :: "r"(p) : "memory");
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(fpsr));
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    *(uint32_t *)&t->fpu_state[512] = fpsr;
    *(uint32_t *)&t->fpu_state[516] = fpcr;
}

static inline void fpu_restore(thread_t *t) {
    uint32_t fpsr = *(uint32_t *)&t->fpu_state[512];
    uint32_t fpcr = *(uint32_t *)&t->fpu_state[516];
    uint64_t *p = (uint64_t *)t->fpu_state;
    __asm__ __volatile__(
        "ldp q0,  q1,  [%0, #0]\n\t"
        "ldp q2,  q3,  [%0, #32]\n\t"
        "ldp q4,  q5,  [%0, #64]\n\t"
        "ldp q6,  q7,  [%0, #96]\n\t"
        "ldp q8,  q9,  [%0, #128]\n\t"
        "ldp q10, q11, [%0, #160]\n\t"
        "ldp q12, q13, [%0, #192]\n\t"
        "ldp q14, q15, [%0, #224]\n\t"
        "ldp q16, q17, [%0, #256]\n\t"
        "ldp q18, q19, [%0, #288]\n\t"
        "ldp q20, q21, [%0, #320]\n\t"
        "ldp q22, q23, [%0, #352]\n\t"
        "ldp q24, q25, [%0, #384]\n\t"
        "ldp q26, q27, [%0, #416]\n\t"
        "ldp q28, q29, [%0, #448]\n\t"
        "ldp q30, q31, [%0, #480]\n\t"
        :: "r"(p) : "memory");
    __asm__ __volatile__("msr fpsr, %0" :: "r"(fpsr));
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr));
}

static void idle_entry(void) {
    while (1) {
        __asm__ __volatile__("wfi");
    }
}

static int setup_idle(void) {
    idle_thread = (thread_t *)pmm_alloc_pages(1);
    if (!idle_thread) return -1;

    uint64_t *p = (uint64_t *)idle_thread;
    for (int i = 0; i < 4096 / 8; i++) p[i] = 0;

    idle_thread->tid = 0;
    idle_thread->state = THREAD_READY;
    idle_thread->static_prio = PRIO_MAX - 1;
    idle_thread->prio = PRIO_MAX - 1;
    idle_thread->time_slice = 1;
    idle_thread->sleep_avg = 0;
    idle_thread->page_dir = 0;
    idle_thread->kernel_stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!idle_thread->kernel_stack) return -1;
    idle_thread->kernel_stack_top = (uint64_t)idle_thread->kernel_stack + THREAD_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(idle_thread->kernel_stack_top - sizeof(registers_t));
    for (int i = 0; i < 30; i++) frame->x[i] = 0;
    frame->lr = 0;
    frame->sp = 0;
    frame->far = 0;
    frame->esr = 0;
    frame->elr = (uint64_t)idle_entry;
    frame->spsr = 0x345;

    idle_thread->kernel_rsp = (uint64_t)frame;
    return 0;
}

static void *context_switch(registers_t *r) {
    if (active->nr_active == 0) {
        prio_array_t *tmp = active;
        active  = expired;
        expired = tmp;
    }

    int top_prio = prio_array_find_top(active);
    if (top_prio < 0) {
        uart_print("sw: IDLE\n");
        if (current_thread)
            fpu_save(current_thread);
        current_thread = idle_thread;
        return (void *)idle_thread->kernel_rsp;
    }

    thread_t *next = active->queue[top_prio];
    prio_array_dequeue(active, next, top_prio);

    if (next->state == THREAD_ZOMBIE || next->state == THREAD_UNUSED)
        return context_switch(r);

    /* The frame at next->kernel_rsp already holds the thread's own
     * saved state: prepared by thread_create()/arch_fork_setup_regs()
     * for fresh threads, or saved by scheduler_switch() for threads
     * that have run before.  Do NOT copy the interrupted thread's
     * live registers over it — that clobbers the child's fork frame
     * (x0/sp/callee-saved) and restarts run-before threads from
     * their original entry instead of resuming them. */
    next->state = THREAD_RUNNING;
    if (current_thread)
        fpu_save(current_thread);
    current_thread = next;
    fpu_restore(next);

    if (next->page_dir)
        vmm_switch_directory((page_directory_t *)next->page_dir);

    return (void *)next->kernel_rsp;
}

void *scheduler_switch(registers_t *r) {
    ticks++;
    sleep_wake_tick();

    for (int pass = 0; pass < 2; pass++) {
        prio_array_t *pa = (pass == 0) ? active : expired;
        for (int i = 0; i < PRIO_ARRAY_BITS; i++) {
            thread_t *t = pa->queue[i];
            if (!t) continue;
            thread_t *start = t;
            do {
                if (t->state == THREAD_BLOCKED && t->sleep_until && t->sleep_until <= ticks) {
                    t->state = THREAD_READY;
                    t->sleep_until = 0;
                    if (t->sleep_avg < MAX_SLEEP_AVG)
                        t->sleep_avg += 5;
                    if (t->sleep_avg > MAX_SLEEP_AVG)
                        t->sleep_avg = MAX_SLEEP_AVG;
                }
                t = t->next;
            } while (t != start);
        }
    }

    if (current_thread && current_thread->tid != 0 && current_thread->state == THREAD_RUNNING) {
        current_thread->kernel_rsp = (uint64_t)r;

        if (current_thread->time_slice > 0)
            current_thread->time_slice--;

        if (current_thread->time_slice == 0) {
            current_thread->state = THREAD_READY;

            if (current_thread->sleep_avg > 0)
                current_thread->sleep_avg -= 3;
            if (current_thread->sleep_avg < 0)
                current_thread->sleep_avg = 0;

            current_thread->prio = effective_prio(current_thread->static_prio,
                                                  current_thread->sleep_avg);
            current_thread->time_slice = prio_to_timeslice(current_thread->static_prio);

            prio_array_enqueue(expired, current_thread, current_thread->prio);
        } else {
            /* Preempted with time remaining: back to the active queue
             * with the rest of the slice.  Dropping the thread here
             * (as before) silently removed it from all queues. */
            current_thread->state = THREAD_READY;
            prio_array_enqueue(active, current_thread, current_thread->prio);
        }
        current_thread = 0;
    }

    return context_switch(r);
}


/* Blocking + timed sleep                                              */

void scheduler_block_current(void) {
    thread_t *cur = current_thread;
    if (!cur || cur == idle_thread)
        return;

    if (cur->array)
        prio_array_dequeue(cur->array, cur, cur->prio);
    cur->state = THREAD_BLOCKED;
}

static void sleep_wake_tick(void) {
    uint32_t now = (uint32_t)clockevent_get_ticks();
    for (int i = 0; i < sleep_count; i++) {
        thread_t *t = sleep_queue[i];
        if (t && t->sleep_until && (int32_t)(now - t->sleep_until) >= 0) {
            sleep_queue[i] = sleep_queue[--sleep_count];
            t->sleep_until = 0;
            scheduler_unblock_thread(t);
            i--;
        }
    }
}

int scheduler_sleep_ticks(uint64_t deadline) {
    thread_t *cur = current_thread;
    if (!cur || cur == idle_thread)
        return -1;

    if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
        return 0;

    if (cur->array)
        prio_array_dequeue(cur->array, cur, cur->prio);
    cur->sleep_until = (uint32_t)deadline;
    cur->state = THREAD_BLOCKED;

    int added = (sleep_count < SLEEP_MAX);
    if (added)
        sleep_queue[sleep_count++] = cur;

    if (!added) {
        cur->sleep_until = 0;
        cur->state = THREAD_READY;
        scheduler_add_thread(cur);
        return -1;
    }

    /* arm64 has no thread_yield; the tick path (scheduler_switch) sees
     * THREAD_BLOCKED and switches away.  When the deadline expires the
     * sleep scan wakes us and we resume right after WFI. */
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
    __asm__ __volatile__("wfi");
    return 0;
}

void scheduler_init(void) {
    for (int i = 0; i < 5; i++) {
        active_array.bitmap[i] = 0;
        expired_array.bitmap[i] = 0;
    }
    active_array.nr_active = 0;
    expired_array.nr_active = 0;
    for (int i = 0; i < PRIO_ARRAY_BITS; i++) {
        active_array.queue[i] = 0;
        expired_array.queue[i] = 0;
    }
    current_thread = 0;

    if (setup_idle() < 0)
        return;

    idle_thread->state = THREAD_RUNNING;
    current_thread = idle_thread;
    uart_print("scheduler: O(1) init done\n");
}

void scheduler_add_thread(thread_t *thread) {
    if (!thread) return;

    if (thread->static_prio == 0)
        thread->static_prio = DEFAULT_PRIO;
    if (thread->prio == 0)
        thread->prio = thread->static_prio;
    if (thread->time_slice == 0)
        thread->time_slice = prio_to_timeslice(thread->static_prio);

    thread->state = THREAD_READY;
    prio_array_enqueue(active, thread, thread->prio);
}

void scheduler_remove_thread(thread_t *thread) {
    if (!thread || !thread->array) return;

    if (thread->array) {
        prio_array_dequeue(thread->array, thread, thread->prio);
        thread->state = THREAD_ZOMBIE;
    }
}

void scheduler_unblock_thread(thread_t *thread) {
    if (!thread) return;

    if (thread->state == THREAD_BLOCKED) {
        thread->state = THREAD_READY;
        if (thread->sleep_avg < MAX_SLEEP_AVG)
            thread->sleep_avg += 10;
        if (thread->sleep_avg > MAX_SLEEP_AVG)
            thread->sleep_avg = MAX_SLEEP_AVG;

        thread->prio = effective_prio(thread->static_prio, thread->sleep_avg);
        prio_array_enqueue(active, thread, thread->prio);
    }
}

thread_t *scheduler_current_thread(void) {
    return current_thread;
}

/* Yield the CPU from kernel-thread context.
 * Called from blocking paths (waitq_sleep, waitq_sleep_timeout) after
 * the current thread was pulled off the runqueue.  WFI must run with
 * the IRQ mask clear: exceptions to EL1 set PSTATE.I, so without an
 * explicit daifclr the timer IRQ can never wake us and the whole
 * system freezes on the first blocking syscall.  Do NOT touch the
 * thread state here — scheduler_block_current() already marked it
 * THREAD_BLOCKED, and scheduler_unblock_thread() only re-enqueues
 * threads whose state is still THREAD_BLOCKED. */
void thread_yield(void) {
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
    __asm__ __volatile__("wfi");
}
