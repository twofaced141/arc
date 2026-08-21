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
#include "gdt.h"
#include "debug.h"
#include "string.h"
#include "vmm.h"
#include "spinlock.h"
#include "clockevent.h"
#include "cpu.h"


/* One O(1) runqueue per CPU (SMP Phase 12): each CPU schedules its own
 * threads and runs its own idle thread; threads stay on the runqueue
 * they were added to, and an idle CPU steals work from other CPUs.
 * All queue/current/idle mutation happens under the owning rq's lock
 * (irqsave), which makes cross-CPU wake/remove/steal safe. */
static struct runqueue rqs[CPU_MAX];

/* T7 debug: set while a syscall is blocking via thread_yield */
volatile int sched_dbg = 0;

static inline struct runqueue *rq_current(void) {
    struct cpu *c = cpu_current();
    return (c && c->runqueue) ? c->runqueue : NULL;
}

/* Pick the least-loaded online CPU's runqueue for new thread placement.
 * Scans online CPUs' runqueues without holding locks (racy read of
 * nr_active/sleep_count is acceptable for a placement heuristic).
 * Tie-breaks by smallest CPU id. Falls back to rqs[0] if no online CPU
 * has a runqueue. */
static struct runqueue *pick_least_loaded_rq(void) {
    struct runqueue *best = NULL;
    unsigned best_load = ~0u;
    unsigned best_id = CPU_MAX;

    for (unsigned i = 0; i < cpu_nr; i++) {
        struct cpu *c = &cpus[i];
        struct runqueue *rq = c->runqueue;
        if (!rq || c->state != CPU_ONLINE)
            continue;

        unsigned load = rq->active->nr_active + rq->expired->nr_active + (unsigned)rq->sleep_count;
        if (load < best_load || (load == best_load && i < best_id)) {
            best = rq;
            best_load = load;
            best_id = i;
        }
    }

    if (!best)
        best = &rqs[0];
    return best;
}

static inline int __ffs(uint32_t word) {
    int r;
    __asm__("bsf %1, %0" : "=r"(r) : "r"(word));
    return r;
}


static void prio_array_enqueue(struct runqueue *rq, prio_array_t *pa,
                               thread_t *t, int prio) {
    (void)rq;
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
#ifdef CONFIG_DEBUG
    debug_printf("enq:%c tid=%u p=%u n=%u\r\n",
                 pa == rq->active ? 'A' : 'E',
                 t->tid, (unsigned)prio,
                 (unsigned)pa->nr_active);
#endif
}

static void prio_array_dequeue(struct runqueue *rq, prio_array_t *pa,
                               thread_t *t, int prio) {
    (void)rq;
    if (t->next == t) {
        pa->queue[prio] = NULL;
        pa->bitmap[prio / 32] &= ~(1u << (prio % 32));
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (pa->queue[prio] == t)
            pa->queue[prio] = t->next;
    }
    t->next = NULL;
    t->prev = NULL;
    t->array = NULL;
    pa->nr_active--;
#ifdef CONFIG_DEBUG
    debug_printf("deq:%c tid=%u p=%u n=%u\r\n",
                 pa == rq->active ? 'A' : 'E',
                 t->tid, (unsigned)prio,
                 (unsigned)pa->nr_active);
#endif
}

static int prio_array_find_top(const prio_array_t *pa) {
    for (int i = 0; i < BITMAP_SIZE; i++) {
        if (pa->bitmap[i]) {
            return i * 32 + __ffs(pa->bitmap[i]);
        }
    }
    return -1;
}


#define DEFAULT_PRIO 120

static inline int nice_to_prio(int nice) {
    int p = DEFAULT_PRIO + nice;
    if (p < MAX_RT_PRIO) p = MAX_RT_PRIO;
    if (p >= PRIO_MAX)   p = PRIO_MAX - 1;
    return p;
}

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

/* CR0.TS is set on every context switch, so the first x87/SSE/MMX     */
/* instruction of a thread traps (#NM).  The #NM handler captures the  */
/* previous owner's live FPU state with fxsave and restores the fault- */
/* ing thread's state with fxrstor.  The kernel itself never executes  */
/* FPU instructions, so a #NM can only come from a thread's own use.   */
/* The owner is tracked per CPU (a thread's FPU state only matters on  */
/* the CPU it currently runs on).                                      */

static inline void fpu_save(thread_t *t) {
    __asm__ __volatile__("fxsaveq (%0)" :: "r"(t->fpu_state) : "memory");
}

static inline void fpu_restore(thread_t *t) {
    __asm__ __volatile__("fxrstorq (%0)" :: "r"(t->fpu_state) : "memory");
}

static inline void fpu_clts(void) {
    __asm__ __volatile__("clts" ::: "memory");
}

/* #NM (int 7): thread's first FPU use after a switch. */
void fpu_nm_handler(registers_t *r) {
    (void)r;
    struct runqueue *rq = rq_current();
    thread_t *cur = rq ? rq->current : NULL;

    fpu_clts();
    if (rq && rq->fpu_owner && rq->fpu_owner != cur)
        fpu_save(rq->fpu_owner);
    if (cur && cur->tid != 0) {
        fpu_restore(cur);
        rq->fpu_owner = cur;
    }
}


/* Idle thread                                                        */

static void idle_entry(void) {
    while (1) {
        __asm__ __volatile__("sti; hlt");
    }
}

static int setup_idle(struct runqueue *rq) {
    thread_t *idle = (thread_t *)kmalloc(sizeof(thread_t));
    if (!idle) return -1;

    memset(idle, 0, sizeof(thread_t));
    thread_fpu_state_init(idle->fpu_state);
    idle->tid = 0;
    idle->state = THREAD_READY;
    idle->static_prio = PRIO_MAX - 1;
    idle->prio = PRIO_MAX - 1;
    idle->time_slice = 1;
    idle->sleep_avg = 0;
    idle->page_dir = NULL;
    idle->kernel_stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!idle->kernel_stack) return -1;
    idle->kernel_stack_top = (uint64_t)idle->kernel_stack + THREAD_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(idle->kernel_stack_top - sizeof(registers_t) - 8);
    frame->rax = 0; frame->rbx = 0; frame->rcx = 0; frame->rdx = 0;
    frame->rsi = 0; frame->rdi = 0; frame->rbp = 0;
    frame->r8 = 0; frame->r9 = 0; frame->r10 = 0; frame->r11 = 0;
    frame->r12 = 0; frame->r13 = 0; frame->r14 = 0; frame->r15 = 0;
    frame->int_no = 0; frame->err_code = 0;
    frame->rip = (uint64_t)idle_entry;
    frame->cs = 0x08;
    frame->rflags = 0x202;
    frame->rsp = (uint64_t)&frame->int_no;
    frame->ss = 0x10;

    idle->kernel_rsp = (uint64_t)frame;
    rq->idle = idle;
    return 0;
}

static void rq_init(struct runqueue *rq) {
    memset(rq, 0, sizeof(*rq));
    rq->active = &rq->arrays[0];
    rq->expired = &rq->arrays[1];
}

static void *context_switch(struct runqueue *rq, registers_t *r) {
    (void)r;

    if (rq->active->nr_active <= 0) {
        prio_array_t *tmp = rq->active;
        rq->active  = rq->expired;
        rq->expired = tmp;
    }

    int top_prio = prio_array_find_top(rq->active);
    if (top_prio < 0) {
        if (sched_dbg)
            debug_printf("SW -> idle (krs=%lx)\r\n",
                         (unsigned long)rq->idle->kernel_rsp);
        rq->current = rq->idle;
        tss_set_kernel_stack(rq->idle->kernel_stack_top);
        return (void *)rq->idle->kernel_rsp;
    }

    thread_t *next = rq->active->queue[top_prio];
    if (sched_dbg)
        debug_printf("SW -> tid=%u (krs=%lx st=%u)\r\n",
                     next->tid, (unsigned long)next->kernel_rsp, next->state);
    if (next->next == next) {
        /* Single thread — actually remove it from active. */
        rq->active->queue[top_prio] = NULL;
        rq->active->bitmap[top_prio / 32] &= ~(1u << (top_prio % 32));
        next->array = NULL;
        rq->active->nr_active--;
    } else {
        next->prev->next = next->next;
        next->next->prev = next->prev;
        rq->active->queue[top_prio] = next->next;
        thread_t *tail = next->next->prev;
        next->prev = tail;
        tail->next = next;
        next->next->prev = next;
        next->array = rq->active;
    }

    if (next->state == THREAD_ZOMBIE || next->state == THREAD_UNUSED ||
        next->state == THREAD_BLOCKED)
        return context_switch(rq, r);

    next->state = THREAD_RUNNING;
    rq->current = next;

    /* Per-thread TLS base: the syscall/interrupt entries load fs with a
     * null base, so restore FSBASE for whatever thread is next up.
     * Kernel threads and the idle thread run with FSBASE = 0. */
    {
        uint64_t base = next->tid ? (uint64_t)next->tls_base : 0;
        uint32_t lo = (uint32_t)base, hi = (uint32_t)(base >> 32);
        __asm__ __volatile__("wrmsr" : : "c"(0xC0000100), "a"(lo), "d"(hi));
    }

    {
        uint64_t cr0;
        __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= (1 << 3);
        __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
    }
    tss_set_kernel_stack(next->kernel_stack_top);
    if (next->page_dir)
        vmm_switch_directory(next->page_dir);
    return (void *)next->kernel_rsp;
}


/* Scheduler switch                                                   */

static void sleep_wake_tick(struct runqueue *rq);

/* Called from .yield_resume (interrupts.s) right after the resume
 * iretq, with %rdi = RSP as left by iretq.  For a kernel->kernel
 * (same-ring) return this is frame+160; if iretq wrongly popped the
 * saved SS:RSP slots it is the frame's saved RSP value instead.  The
 * `ret` that follows pops [rsp+16] after .yield_resume's add $16. */
void yield_resume_probe(void *rsp_after_iretq) {
    if (!sched_dbg)
        return;
    uint64_t *p = (uint64_t *)rsp_after_iretq;
    debug_printf("YR rsp=%lx [0]=%lx [1]=%lx [2]=%lx\r\n",
                 (unsigned long)(uintptr_t)rsp_after_iretq,
                 (unsigned long)p[0], (unsigned long)p[1],
                 (unsigned long)p[2]);
}

/* Phase 12 idle balancing: steal the highest-priority thread from a
 * remote CPU's queue into ours.  Never holds two rq locks at once
 * (remote lock is dropped before the local enqueue), so no lock
 * ordering is introduced between CPUs. */
static void rq_steal(struct runqueue *rq) {
    if (rq->active->nr_active > 0)
        return;

    for (unsigned i = 0; i < cpu_nr; i++) {
        if (i == rq->cpu_id)
            continue;
        struct cpu *c = &cpus[i];
        struct runqueue *srq = c->runqueue;
        if (!srq || c->state != CPU_ONLINE)
            continue;

        uint32_t sflags;
        spin_lock_irqsave(&srq->lock, &sflags);

        prio_array_t *pa = srq->active;
        int top = prio_array_find_top(pa);
        if (top < 0) {
            pa = srq->expired;
            top = prio_array_find_top(pa);
        }
        if (top < 0) {
            spin_unlock_irqrestore(&srq->lock, sflags);
            continue;
        }

        thread_t *t = pa->queue[top];
        prio_array_dequeue(srq, pa, t, top);
        spin_unlock_irqrestore(&srq->lock, sflags);

        t->rq = rq;
        t->state = THREAD_READY;
        uint32_t lflags;
        spin_lock_irqsave(&rq->lock, &lflags);
        prio_array_enqueue(rq, rq->active, t, t->prio);
        spin_unlock_irqrestore(&rq->lock, lflags);
        if (sched_dbg)
            debug_printf("STEAL cpu%u <- tid=%u\r\n", rq->cpu_id, t->tid);
        return;
    }
}

void *scheduler_switch(registers_t *r) {
    /* No nested interrupts while the switch is in flight: a tick landing
     * between context_switch() and the stub's stack swap would write
     * current_thread->kernel_rsp with a frame on the OLD stack.  The
     * resume path (iretq/thread_yield) restores RFLAGS, so IF comes back. */
    __asm__ __volatile__("cli");

    struct cpu *cpu = cpu_current();
    struct runqueue *rq = cpu ? cpu->runqueue : NULL;
    if (!rq)
        return (void *)r;

    uint64_t int_no = r->int_no;

    if (rq->current && rq->current->kernel_stack) {
        uint64_t lo = (uint64_t)(uintptr_t)rq->current->kernel_stack;
        uint64_t hi = rq->current->kernel_stack_top;
        if (rq->current->kernel_rsp < lo || rq->current->kernel_rsp > hi)
            log_printf(LOG_LEVEL_ERROR, "SW-CHK BAD tid=%u rsp=0x%lx stack=[0x%lx..0x%lx]\r\n",
                         rq->current->tid, rq->current->kernel_rsp, lo, hi);
    }

    /* A pending IPI_RESCHEDULE is satisfied by any preemption; the tick
     * path always switches, so it may consume the flag too. */
    if (int_no == 32)
        cpu->arch.need_resched = 0;

    if (rq->active->nr_active <= 0)
        rq_steal(rq);

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);

    if (int_no == 32) {
        sleep_wake_tick(rq);

        if (rq->current && rq->current->tid != 0) {
            if (sched_dbg)
                debug_printf("SW32 store tid=%u krs=%lx int=%u\r\n",
                             rq->current->tid,
                             (unsigned long)(uintptr_t)r, int_no);
            rq->current->kernel_rsp = (uint64_t)r;

            /* Preemption only applies to RUNNING threads, but the frame
             * must be saved for any thread currently on the CPU: a tick
             * can land between scheduler_sleep_ticks() (thread marked
             * THREAD_BLOCKED and pulled off the runqueue) and
             * thread_yield()'s own switch.  If kernel_rsp is not saved
             * then, the later wakeup resumes the thread at its stale
             * syscall frame and the sleep syscall never completes. */
            if (rq->current->state == THREAD_RUNNING) {
                if (rq->current->time_slice > 0)
                    rq->current->time_slice--;

                if (rq->current->time_slice == 0) {
                    rq->current->state = THREAD_READY;

                    if (rq->current->sleep_avg > 0)
                        rq->current->sleep_avg -= 3;
                    if (rq->current->sleep_avg < 0)
                        rq->current->sleep_avg = 0;

                    rq->current->prio = effective_prio(rq->current->static_prio,
                                                       rq->current->sleep_avg);
                    rq->current->time_slice = prio_to_timeslice(rq->current->static_prio);

                    if (rq->current->array == rq->active)
                        prio_array_dequeue(rq, rq->active, rq->current, rq->current->prio);
                    prio_array_enqueue(rq, rq->expired, rq->current, rq->current->prio);
                    rq->current = NULL;
                } else {
                    if (rq->current->array == NULL)
                        prio_array_enqueue(rq, rq->active, rq->current, rq->current->prio);
                    rq->current = NULL;
                }
            }
        }

        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    if (int_no == 128) {
        if (rq->current && rq->current->tid != 0) {
            if (sched_dbg)
                debug_printf("SW128 store tid=%u krs=%lx\r\n",
                             rq->current->tid, (unsigned long)(uintptr_t)r);
            rq->current->kernel_rsp = (uint64_t)r;

            if (rq->current->state == THREAD_RUNNING) {
                int old_prio = rq->current->prio;
                rq->current->state = THREAD_READY;

                if (rq->current->sleep_avg > 0)
                    rq->current->sleep_avg--;

                rq->current->prio = effective_prio(rq->current->static_prio,
                                                   rq->current->sleep_avg);

                if (rq->current->time_slice > 0)
                    rq->current->time_slice--;

                if (rq->current->time_slice == 0) {
                    if (rq->current->array == rq->active)
                        prio_array_dequeue(rq, rq->active, rq->current, old_prio);
                    prio_array_enqueue(rq, rq->expired, rq->current, rq->current->prio);
                } else {
                    if (rq->current->array == NULL) {
                        prio_array_enqueue(rq, rq->active, rq->current, rq->current->prio);
                    }
                }
                rq->current = NULL;
            }
        }

        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    if (rq->current && rq->current->tid != 0) {
        if (sched_dbg) {
            uint64_t *fr = (uint64_t *)r;
            uint64_t retslot = *(uint64_t *)((char *)r + 176);
            debug_printf("SWyld store tid=%u krs=%lx int=%u "
                         "fr[int]=%lx fr[rip]=%lx fr[cs]=%lx fr[rfl]=%lx "
                         "fr[rsp]=%lx retslot=%lx\r\n",
                         rq->current->tid,
                         (unsigned long)(uintptr_t)r, int_no,
                         (unsigned long)fr[15], (unsigned long)fr[17],
                         (unsigned long)fr[18], (unsigned long)fr[19],
                         (unsigned long)fr[20], (unsigned long)retslot);
        }
        rq->current->kernel_rsp = (uint64_t)r;
    }

#ifdef CONFIG_DEBUG
    if (int_no == 0 && rq->current) {
        /* Yield path: r->rip is .yield_resume; the caller's return
         * address sits at frame+176 (after 15 regs, int_no, err_code,
         * rip, cs, rflags, rsp_slot, ss).  Print it if it looks wrong. */
        uint64_t retaddr = *(uint64_t *)((char *)r + 176);
        if (retaddr < 0x100000 || retaddr > 0x116000)
            debug_printf("yield BAD tid=%u frame=%p ret=%lx\r\n",
                         rq->current->tid, (void *)r, retaddr);
    }
#endif

    if (!rq->current) {
        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    if (rq->current->state != THREAD_RUNNING) {
        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    if (cpu->arch.need_resched) {
        cpu->arch.need_resched = 0;
        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    spin_unlock_irqrestore(&rq->lock, flags);
    return (void *)r;
}


/* Blocking + timed sleep                                              */

/* Mark the calling thread BLOCKED and remove it from the runqueue so
 * a later scheduler_unblock_thread() re-enqueues it exactly once. */
void scheduler_block_current(void) {
    struct runqueue *rq = rq_current();
    thread_t *cur = rq ? rq->current : NULL;
    if (!rq || !cur || cur == rq->idle)
        return;

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);
    if (cur->array)
        prio_array_dequeue(rq, cur->array, cur, cur->prio);
    cur->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&rq->lock, flags);
}

/* Wake helper — caller must hold rq->lock. */
static void rq_unblock_locked(struct runqueue *rq, thread_t *thread) {
    if (thread->state != THREAD_BLOCKED)
        return;

    thread->state = THREAD_READY;
    if (thread->sleep_avg < MAX_SLEEP_AVG)
        thread->sleep_avg += 10;
    if (thread->sleep_avg > MAX_SLEEP_AVG)
        thread->sleep_avg = MAX_SLEEP_AVG;

    thread->prio = effective_prio(thread->static_prio, thread->sleep_avg);
    prio_array_enqueue(rq, rq->active, thread, thread->prio);
}

/* Wake every sleeper whose deadline has passed.  Called from the timer
 * tick (IRQ context, IF off, rq->lock held). */
static void sleep_wake_tick(struct runqueue *rq) {
    uint32_t now = (uint32_t)clockevent_get_ticks();
    for (int i = 0; i < rq->sleep_count; i++) {
        thread_t *t = rq->sleep_queue[i];
        if (t && t->sleep_until && (int32_t)(now - t->sleep_until) >= 0) {
            rq->sleep_queue[i] = rq->sleep_queue[--rq->sleep_count];
            t->sleep_until = 0;
            rq_unblock_locked(rq, t);
            i--;
        }
    }
}

/* Block the calling thread until clockevent_get_ticks() reaches
 * deadline.  Returns 1 if the thread was blocked (the caller must
 * thread_yield() and wait for the deadline or a signal wakeup),
 * 0 if the deadline already passed (no blocking needed), -1 if it
 * could not sleep (idle thread or sleep queue full). */
int scheduler_sleep_ticks(uint64_t deadline) {
    struct runqueue *rq = rq_current();
    thread_t *cur = rq ? rq->current : NULL;
    if (!rq || !cur || cur == rq->idle)
        return -1;

    if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
        return 0;

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);

    if (cur->array)
        prio_array_dequeue(rq, cur->array, cur, cur->prio);
    cur->sleep_until = (uint32_t)deadline;
    cur->state = THREAD_BLOCKED;

    int added = (rq->sleep_count < SLEEP_MAX);
    if (added)
        rq->sleep_queue[rq->sleep_count++] = cur;

    spin_unlock_irqrestore(&rq->lock, flags);

    if (!added) {
        cur->sleep_until = 0;
        cur->state = THREAD_READY;
        scheduler_add_thread(cur);
        return -1;
    }

    if (sched_dbg)
        debug_printf("SLEEP block tid=%u now=%u dl=%u\r\n",
                     cur->tid, (unsigned)clockevent_get_ticks(),
                     (unsigned)deadline);
    return 1;
}


/* Public API                                                         */
/* ------------------------------------------------------------------ */

void scheduler_init(void) {
    struct cpu *bsp = &cpus[0];
    struct runqueue *rq = &rqs[0];

    bsp->runqueue = rq;
    rq->cpu_id = 0;
    rq_init(rq);

    if (setup_idle(rq) < 0)
        return;

    rq->idle->state = THREAD_RUNNING;
    rq->current = rq->idle;
    bsp->current = rq->idle;
    bsp->idle = rq->idle;
    register_interrupt_handler(7, fpu_nm_handler);
    log_print(LOG_LEVEL_DEBUG, "scheduler: per-CPU O(1) runqueues ready\n");
}

/* Per-CPU scheduler init (SMP Phase 12): called on an AP right after
 * arch_percpu_init() (GS set).  The AP gets its own runqueue and idle
 * thread; the trampoline stack is abandoned on the first switch, same
 * as the BSP's boot stack. */
void scheduler_init_cpu(struct cpu *cpu) {
    struct runqueue *rq = &rqs[cpu->id];

    cpu->runqueue = rq;
    rq->cpu_id = cpu->id;
    rq_init(rq);

    if (setup_idle(rq) < 0) {
        log_printf(LOG_LEVEL_ERROR, "scheduler: cpu %u idle setup failed\r\n",
                   cpu->id);
        return;
    }

    rq->idle->state = THREAD_RUNNING;
    rq->current = rq->idle;
    cpu->current = rq->idle;
    cpu->idle = rq->idle;
    log_printf(LOG_LEVEL_DEBUG, "scheduler: cpu %u runqueue ready\r\n", cpu->id);
}

void scheduler_add_thread(thread_t *thread) {
    if (!thread) return;

    if (thread->static_prio == 0 && thread->tid != 0)
        thread->static_prio = DEFAULT_PRIO;
    if (thread->prio == 0)
        thread->prio = thread->static_prio;
    if (thread->time_slice == 0)
        thread->time_slice = prio_to_timeslice(thread->static_prio);

    struct runqueue *rq = pick_least_loaded_rq();

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);
    thread->state = THREAD_READY;
    thread->rq = rq;
    prio_array_enqueue(rq, rq->active, thread, thread->prio);
    spin_unlock_irqrestore(&rq->lock, flags);
}

void scheduler_remove_thread(thread_t *thread) {
    if (!thread || !thread->array) return;

    struct runqueue *rq = thread->rq;
    if (!rq) return;

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);

    if (thread->array) {
        prio_array_dequeue(rq, thread->array, thread, thread->prio);
        thread->state = THREAD_ZOMBIE;
    }

    spin_unlock_irqrestore(&rq->lock, flags);
}

void scheduler_unblock_thread(thread_t *thread) {
    if (!thread) return;

    struct runqueue *rq = thread->rq ? thread->rq : rq_current();
    if (!rq) return;

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);
    rq_unblock_locked(rq, thread);
    spin_unlock_irqrestore(&rq->lock, flags);

    /* Cross-CPU wake: nudge the home CPU so it preempts promptly
     * (Phase 11 IPI_RESCHEDULE; honored in scheduler_switch). */
    if (rq != rq_current() && cpu_online(cpu_get(rq->cpu_id)))
        cpu_send_ipi(cpu_get(rq->cpu_id), IPI_RESCHEDULE);
}

/* Migrate a thread to a target CPU's runqueue.
 * Returns 0 on success, -1 on failure (invalid args, thread running on
 * its current CPU, thread blocked/sleeping, target CPU offline). */
int thread_migrate(thread_t *thread, unsigned target_cpu) {
    if (!thread)
        return -1;
    if (target_cpu >= cpu_nr || target_cpu >= CPU_MAX)
        return -1;

    struct cpu *tcpu = cpu_get(target_cpu);
    if (!tcpu || tcpu->state != CPU_ONLINE || !tcpu->runqueue)
        return -1;

    struct runqueue *target_rq = tcpu->runqueue;
    struct runqueue *source_rq = thread->rq;

    if (!source_rq)
        return -1;

    /* Already on target runqueue. */
    if (source_rq == target_rq)
        return 0;

    /* Cannot migrate a thread that is currently RUNNING on its CPU.
     * The thread must be READY (on a runqueue) or we reject. */
    if (thread->state == THREAD_RUNNING) {
        struct runqueue *cur_rq = rq_current();
        if (cur_rq && cur_rq->current == thread)
            return -1;
    }

    /* Do not migrate threads that are in the sleep queue (BLOCKED with
     * a deadline).  They are not on a prio array and have sleep_until set. */
    if (thread->state == THREAD_BLOCKED && thread->sleep_until)
        return -1;

    /* Dequeue from source runqueue. */
    uint32_t sflags;
    spin_lock_irqsave(&source_rq->lock, &sflags);

    if (thread->array) {
        prio_array_dequeue(source_rq, thread->array, thread, thread->prio);
    }
    thread->state = THREAD_READY;
    thread->rq = target_rq;

    spin_unlock_irqrestore(&source_rq->lock, sflags);

    /* Enqueue on target runqueue. */
    uint32_t tflags;
    spin_lock_irqsave(&target_rq->lock, &tflags);

    prio_array_enqueue(target_rq, target_rq->active, thread, thread->prio);

    spin_unlock_irqrestore(&target_rq->lock, tflags);

    /* Cross-CPU wake: nudge the target CPU so it preempts promptly. */
    if (target_rq != rq_current() && cpu_online(tcpu))
        cpu_send_ipi(tcpu, IPI_RESCHEDULE);

    return 0;
}

thread_t *scheduler_current_thread(void) {
    struct runqueue *rq = rq_current();
    return rq ? rq->current : NULL;
}

void sched_dump_stats(void) {
    for (unsigned i = 0; i < cpu_nr; i++) {
        struct cpu *c = &cpus[i];
        struct runqueue *rq = c->runqueue;
        if (!rq) {
            log_printf(LOG_LEVEL_INFO, "sched: cpu%u no rq (state %d)\n", i, (int)c->state);
            continue;
        }
        log_printf(LOG_LEVEL_INFO, "sched: cpu%u load %u+%u+%d state %d cur tid %u\n",
                   i,
                   rq->active ? (unsigned)rq->active->nr_active : 0,
                   rq->expired ? (unsigned)rq->expired->nr_active : 0,
                   rq->sleep_count,
                   (int)c->state,
                   rq->current ? rq->current->tid : 9999);
    }
}

void scheduler_set_nice(thread_t *t, int nice) {
    if (!t) return;
    if (nice < -20) nice = -20;
    if (nice > 19)  nice = 19;

    struct runqueue *rq = t->rq ? t->rq : rq_current();
    if (!rq) return;

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);

    int new_static = nice_to_prio(nice);
    if (t->array) {
        prio_array_dequeue(rq, t->array, t, t->prio);
        t->static_prio = new_static;
        t->prio = effective_prio(t->static_prio, t->sleep_avg);
        t->time_slice = prio_to_timeslice(t->static_prio);
        prio_array_enqueue(rq, t->array, t, t->prio);
    } else {
        t->static_prio = new_static;
        t->prio = effective_prio(t->static_prio, t->sleep_avg);
    }

    spin_unlock_irqrestore(&rq->lock, flags);
}