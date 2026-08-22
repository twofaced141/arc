/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 */


#include "scheduler.h"
#include "thread.h"
#include "pmm.h"
#include "memory.h"
#include "uart.h"
#include "string.h"
#include "vmm.h"
#include "clockevent.h"
#include "spinlock.h"
#include "cpu.h"

/* One O(1) runqueue per CPU (SMP Phase 12): each CPU schedules its own
 * threads and runs its own idle thread; threads stay on the runqueue
 * they were added to, and an idle CPU steals work from other CPUs. */

static struct runqueue rqs[CPU_MAX];

static inline struct runqueue *rq_current(void) {
    struct cpu *c = cpu_current();
    return (c && c->runqueue) ? c->runqueue : NULL;
}

/* Pick the least-loaded online CPU's runqueue for new thread placement. */
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
    return __builtin_ctz(word);
}

static void prio_array_enqueue(struct runqueue *rq, prio_array_t *pa, thread_t *t, int prio) {
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
}

static void prio_array_dequeue(struct runqueue *rq, prio_array_t *pa, thread_t *t, int prio) {
    (void)rq;
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
}

static int prio_array_find_top(const prio_array_t *pa) {
    for (int i = 0; i < BITMAP_SIZE; i++) {
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

/* AArch64 has no x87-style lazy mechanism in EL1 (no CR0.TS), so the
 * Q0-Q31/FPSR/FPCR state is saved and restored on every context
 * switch. */

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

static int setup_idle(struct runqueue *rq) {
    thread_t *idle = (thread_t *)pmm_alloc_pages(1);
    if (!idle) return -1;

    uint64_t *p = (uint64_t *)idle;
    for (int i = 0; i < 4096 / 8; i++) p[i] = 0;

    idle->tid = 0;
    idle->state = THREAD_READY;
    idle->static_prio = PRIO_MAX - 1;
    idle->prio = PRIO_MAX - 1;
    idle->time_slice = 1;
    idle->sleep_avg = 0;
    idle->page_dir = 0;
    idle->rq = rq;
    idle->kernel_stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!idle->kernel_stack) return -1;
    idle->kernel_stack_top = (uint64_t)idle->kernel_stack + THREAD_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(idle->kernel_stack_top - sizeof(registers_t));
    for (int i = 0; i < 30; i++) frame->x[i] = 0;
    frame->lr = 0;
    frame->sp = 0;
    frame->far = 0;
    frame->esr = 0;
    frame->elr = (uint64_t)idle_entry;
    frame->spsr = 0x345;

    idle->kernel_rsp = (uint64_t)frame;
    rq->idle = idle;
    return 0;
}

static void rq_init(struct runqueue *rq) {
    rq->lock = 0;
    rq->active = &rq->arrays[0];
    rq->expired = &rq->arrays[1];
    for (int i = 0; i < BITMAP_SIZE; i++) {
        rq->arrays[0].bitmap[i] = 0;
        rq->arrays[1].bitmap[i] = 0;
    }
    rq->arrays[0].nr_active = 0;
    rq->arrays[1].nr_active = 0;
    for (int i = 0; i < PRIO_ARRAY_BITS; i++) {
        rq->arrays[0].queue[i] = 0;
        rq->arrays[1].queue[i] = 0;
    }
    rq->current = 0;
    rq->idle = 0;
    rq->sleep_count = 0;
}

static void *context_switch(struct runqueue *rq, registers_t *r) {
    (void)r;
    if (rq->active->nr_active == 0) {
        prio_array_t *tmp = rq->active;
        rq->active  = rq->expired;
        rq->expired = tmp;
    }

    int top_prio = prio_array_find_top(rq->active);
    if (top_prio < 0) {
        if (rq->current)
            fpu_save(rq->current);
        rq->current = rq->idle;
        return (void *)rq->idle->kernel_rsp;
    }

    thread_t *next = rq->active->queue[top_prio];
    prio_array_dequeue(rq, rq->active, next, top_prio);

    if (next->state == THREAD_ZOMBIE || next->state == THREAD_UNUSED)
        return context_switch(rq, r);

    next->state = THREAD_RUNNING;
    if (rq->current)
        fpu_save(rq->current);
    rq->current = next;
    fpu_restore(next);

    if (next->page_dir)
        vmm_switch_directory((page_directory_t *)next->page_dir);

    return (void *)next->kernel_rsp;
}

/* Phase 12 idle balancing: steal the highest-priority thread from a
 * remote CPU's queue into ours. */
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
        return;
    }
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

void *scheduler_switch(registers_t *r) {
    /* Mask IRQ while switching to avoid nested tick overwriting kernel_rsp. */
    __asm__ __volatile__("msr daifset, #2" ::: "memory");

    struct cpu *cpu = cpu_current();
    struct runqueue *rq = cpu ? cpu->runqueue : NULL;
    if (!rq)
        return (void *)r;

    /* Consume pending IPI_RESCHEDULE - any preemption satisfies it. */
    if (cpu)
        cpu->arch.need_resched = 0;

    if (rq->active->nr_active == 0)
        rq_steal(rq);

    uint32_t flags;
    spin_lock_irqsave(&rq->lock, &flags);

    sleep_wake_tick(rq);

    if (rq->current && rq->current->tid != 0) {
        rq->current->kernel_rsp = (uint64_t)r;

        if (rq->current->state == THREAD_RUNNING) {
            if (rq->current->time_slice > 0)
                rq->current->time_slice--;

            if (rq->current->time_slice == 0) {
                rq->current->state = THREAD_READY;
                if (rq->current->sleep_avg > 0)
                    rq->current->sleep_avg -= 3;
                if (rq->current->sleep_avg < 0)
                    rq->current->sleep_avg = 0;
                rq->current->prio = effective_prio(rq->current->static_prio, rq->current->sleep_avg);
                rq->current->time_slice = prio_to_timeslice(rq->current->static_prio);
                if (rq->current->array == rq->active)
                    prio_array_dequeue(rq, rq->active, rq->current, rq->current->prio);
                else if (rq->current->array)
                    prio_array_dequeue(rq, rq->current->array, rq->current, rq->current->prio);
                prio_array_enqueue(rq, rq->expired, rq->current, rq->current->prio);
                rq->current = NULL;
            } else {
                if (rq->current->array == NULL) {
                    rq->current->state = THREAD_READY;
                    prio_array_enqueue(rq, rq->active, rq->current, rq->current->prio);
                } else {
                    rq->current->state = THREAD_READY;
                }
                rq->current = NULL;
            }
        } else if (rq->current->state == THREAD_BLOCKED) {
            rq->current = NULL;
        } else {
            rq->current = NULL;
        }
    } else if (rq->current && rq->current->tid == 0) {
        /* Idle thread not saved — it has its own stack. */
        rq->current = NULL;
    } else if (rq->current) {
        rq->current->kernel_rsp = (uint64_t)r;
        rq->current = NULL;
    }

    /* If current was cleared or idle, pick next. */
    if (!rq->current) {
        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    /* Check need_resched for non-tick IPI path. */
    if (cpu && cpu->arch.need_resched) {
        cpu->arch.need_resched = 0;
        void *nxt = context_switch(rq, r);
        spin_unlock_irqrestore(&rq->lock, flags);
        return nxt;
    }

    spin_unlock_irqrestore(&rq->lock, flags);
    return (void *)r;
}

/* Blocking + timed sleep */

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

    return 1;
}

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
    uart_print("scheduler: per-CPU O(1) runqueues ready\n");
}

void scheduler_init_cpu(struct cpu *cpu) {
    struct runqueue *rq = &rqs[cpu->id];

    cpu->runqueue = rq;
    rq->cpu_id = cpu->id;
    rq_init(rq);

    if (setup_idle(rq) < 0) {
        uart_print("smp: cpu idle setup failed\n");
        return;
    }

    rq->idle->state = THREAD_RUNNING;
    rq->current = rq->idle;
    cpu->current = rq->idle;
    cpu->idle = rq->idle;
    uart_print("smp: cpu runqueue ready\n");
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

    if (rq != rq_current() && cpu_online(cpu_get(rq->cpu_id)))
        cpu_send_ipi(cpu_get(rq->cpu_id), IPI_RESCHEDULE);
}

int thread_migrate(thread_t *thread, unsigned target_cpu) {
    if (!thread) return -1;
    if (target_cpu >= cpu_nr || target_cpu >= CPU_MAX) return -1;

    struct cpu *tcpu = cpu_get(target_cpu);
    if (!tcpu || tcpu->state != CPU_ONLINE || !tcpu->runqueue) return -1;

    struct runqueue *target_rq = tcpu->runqueue;
    struct runqueue *source_rq = thread->rq;

    if (!source_rq) return -1;
    if (source_rq == target_rq) return 0;

    if (thread->state == THREAD_RUNNING) {
        struct runqueue *cur_rq = rq_current();
        if (cur_rq && cur_rq->current == thread)
            return -1;
    }

    if (thread->state == THREAD_BLOCKED && thread->sleep_until)
        return -1;

    uint32_t sflags;
    spin_lock_irqsave(&source_rq->lock, &sflags);
    if (thread->array) {
        prio_array_dequeue(source_rq, thread->array, thread, thread->prio);
    }
    thread->state = THREAD_READY;
    thread->rq = target_rq;
    spin_unlock_irqrestore(&source_rq->lock, sflags);

    uint32_t tflags;
    spin_lock_irqsave(&target_rq->lock, &tflags);
    prio_array_enqueue(target_rq, target_rq->active, thread, thread->prio);
    spin_unlock_irqrestore(&target_rq->lock, tflags);

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
            uart_print("sched: cpu no rq\n");
            continue;
        }
        uart_print("sched: cpu load\n");
        (void)rq;
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

    int new_static = 120 + nice;
    if (new_static < MAX_RT_PRIO) new_static = MAX_RT_PRIO;
    if (new_static >= PRIO_MAX) new_static = PRIO_MAX - 1;
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

void thread_yield(void) {
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
    __asm__ __volatile__("wfi");
}
