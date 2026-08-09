#include "scheduler.h"
#include "thread.h"
#include "gdt.h"
#include "debug.h"
#include "string.h"
#include "vmm.h"
#include "spinlock.h"
#include "clockevent.h"
#include "cpu.h"


static prio_array_t active_array;
static prio_array_t expired_array;
static prio_array_t *active  = &active_array;
static prio_array_t *expired = &expired_array;
static thread_t    *current_thread;
static thread_t    *idle_thread;
static uint32_t ticks;

/* T7 debug: set while a syscall is blocking via thread_yield */
volatile int sched_dbg = 0;
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Sleeping threads (blocked with a wake-up deadline).  A thread that
 * calls scheduler_sleep_ticks() is pulled out of the runqueue and
 * parked here; each timer tick wakes the ones whose deadline has
 * passed via scheduler_unblock_thread(). */
#define SLEEP_MAX 256
static thread_t *sleep_queue[SLEEP_MAX];
static int sleep_count;

static void sleep_wake_tick(void);


static inline int __ffs(uint32_t word) {
    int r;
    __asm__("bsf %1, %0" : "=r"(r) : "r"(word));
    return r;
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
#ifdef CONFIG_DEBUG
    debug_printf("enq:%c tid=%u p=%u n=%u\r\n",
                 pa == &active_array ? 'A' : 'E',
                 t->tid, (unsigned)prio,
                 (unsigned)pa->nr_active);
#endif
}

static void prio_array_dequeue(prio_array_t *pa, thread_t *t, int prio) {
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
                 pa == &active_array ? 'A' : 'E',
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

static thread_t *fpu_owner;

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
    thread_t *cur = current_thread;

    fpu_clts();
    if (fpu_owner && fpu_owner != cur)
        fpu_save(fpu_owner);
    if (cur && cur->tid != 0) {
        fpu_restore(cur);
        fpu_owner = cur;
    }
}


/* Idle thread                                                        */

static void idle_entry(void) {
    while (1) {
        __asm__ __volatile__("sti; hlt");
    }
}

static int setup_idle(void) {
    idle_thread = (thread_t *)kmalloc(sizeof(thread_t));
    if (!idle_thread) return -1;

    memset(idle_thread, 0, sizeof(thread_t));
    thread_fpu_state_init(idle_thread->fpu_state);
    idle_thread->tid = 0;
    idle_thread->state = THREAD_READY;
    idle_thread->static_prio = PRIO_MAX - 1;
    idle_thread->prio = PRIO_MAX - 1;
    idle_thread->time_slice = 1;
    idle_thread->sleep_avg = 0;
    idle_thread->page_dir = NULL;
    idle_thread->kernel_stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!idle_thread->kernel_stack) return -1;
    idle_thread->kernel_stack_top = (uint64_t)idle_thread->kernel_stack + THREAD_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(idle_thread->kernel_stack_top - sizeof(registers_t) - 8);
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

    idle_thread->kernel_rsp = (uint64_t)frame;
    return 0;
}

static void *context_switch(registers_t *r) {
    (void)r;

    if (active->nr_active <= 0) {
        prio_array_t *tmp = active;
        active  = expired;
        expired = tmp;
    }

    int top_prio = prio_array_find_top(active);
    if (top_prio < 0) {
        if (sched_dbg)
            debug_printf("SW -> idle (krs=%lx)\r\n",
                         (unsigned long)idle_thread->kernel_rsp);
        current_thread = idle_thread;
        tss_set_kernel_stack(idle_thread->kernel_stack_top);
        return (void *)idle_thread->kernel_rsp;
    }

    thread_t *next = active->queue[top_prio];
    if (sched_dbg)
        debug_printf("SW -> tid=%u (krs=%lx st=%u)\r\n",
                     next->tid, (unsigned long)next->kernel_rsp, next->state);
    if (next->next == next) {
        /* Single thread — actually remove it from active. */
        active->queue[top_prio] = NULL;
        active->bitmap[top_prio / 32] &= ~(1u << (top_prio % 32));
        next->array = NULL;
        active->nr_active--;
    } else {
        next->prev->next = next->next;
        next->next->prev = next->prev;
        active->queue[top_prio] = next->next;
        thread_t *tail = next->next->prev;
        next->prev = tail;
        tail->next = next;
        next->next->prev = next;
        next->array = active;
    }

    if (next->state == THREAD_ZOMBIE || next->state == THREAD_UNUSED ||
        next->state == THREAD_BLOCKED)
        return context_switch(r);

    next->state = THREAD_RUNNING;
    current_thread = next;

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

void *scheduler_switch(registers_t *r) {
    /* No nested interrupts while the switch is in flight: a tick landing
     * between context_switch() and the stub's stack swap would write
     * current_thread->kernel_rsp with a frame on the OLD stack.  The
     * resume path (iretq/thread_yield) restores RFLAGS, so IF comes back. */
    __asm__ __volatile__("cli");

    /* Global scheduler: only the BSP runs threads.  APs idle in the hlt
     * loop and their interrupt frames live on the per-CPU idle stack —
     * never let scheduler_switch save/switch on them. */
    if (cpu_current() != &cpus[0])
        return (void *)r;

    uint64_t int_no = r->int_no;

    if (current_thread && current_thread->kernel_stack) {
        uint64_t lo = (uint64_t)(uintptr_t)current_thread->kernel_stack;
        uint64_t hi = current_thread->kernel_stack_top;
        if (current_thread->kernel_rsp < lo || current_thread->kernel_rsp > hi)
            log_printf(LOG_LEVEL_ERROR, "SW-CHK BAD tid=%u rsp=0x%lx stack=[0x%lx..0x%lx]\r\n",
                         current_thread->tid, current_thread->kernel_rsp, lo, hi);
    }

    if (int_no == 32) {
        ticks++;
        sleep_wake_tick();

        if (current_thread && current_thread->tid != 0) {
            if (sched_dbg)
                debug_printf("SW32 store tid=%u krs=%lx int=%u\r\n",
                             current_thread->tid,
                             (unsigned long)(uintptr_t)r, int_no);
            current_thread->kernel_rsp = (uint64_t)r;

            /* Preemption only applies to RUNNING threads, but the frame
             * must be saved for any thread currently on the CPU: a tick
             * can land between scheduler_sleep_ticks() (thread marked
             * THREAD_BLOCKED and pulled off the runqueue) and
             * thread_yield()'s own switch.  If kernel_rsp is not saved
             * then, the later wakeup resumes the thread at its stale
             * syscall frame and the sleep syscall never completes. */
            if (current_thread->state == THREAD_RUNNING) {
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

                    if (current_thread->array == active)
                        prio_array_dequeue(active, current_thread, current_thread->prio);
                    prio_array_enqueue(expired, current_thread, current_thread->prio);
                    current_thread = NULL;
                } else {
                    if (current_thread->array == NULL)
                        prio_array_enqueue(active, current_thread, current_thread->prio);
                    current_thread = NULL;
                }
            }
        }

        return context_switch(r);
    }

    if (int_no == 128) {
        uint32_t flags;
        spin_lock_irqsave(&sched_lock, &flags);

        if (current_thread && current_thread->tid != 0) {
            if (sched_dbg)
                debug_printf("SW128 store tid=%u krs=%lx\r\n",
                             current_thread->tid, (unsigned long)(uintptr_t)r);
            current_thread->kernel_rsp = (uint64_t)r;

            if (current_thread->state == THREAD_RUNNING) {
                int old_prio = current_thread->prio;
                current_thread->state = THREAD_READY;

                if (current_thread->sleep_avg > 0)
                    current_thread->sleep_avg--;

                current_thread->prio = effective_prio(current_thread->static_prio,
                                                       current_thread->sleep_avg);

                if (current_thread->time_slice > 0)
                    current_thread->time_slice--;

                if (current_thread->time_slice == 0) {
                    if (current_thread->array == active)
                        prio_array_dequeue(active, current_thread, old_prio);
                    prio_array_enqueue(expired, current_thread, current_thread->prio);
                } else {
                    if (current_thread->array == NULL) {
                        prio_array_enqueue(active, current_thread, current_thread->prio);
                    }
                }
                current_thread = NULL;
            }
        }

        void *next = context_switch(r);
        spin_unlock_irqrestore(&sched_lock, flags);
        return next;
    }

    if (current_thread) {
        if (sched_dbg) {
            uint64_t *fr = (uint64_t *)r;
            uint64_t retslot = *(uint64_t *)((char *)r + 176);
            debug_printf("SWyld store tid=%u krs=%lx int=%u "
                         "fr[int]=%lx fr[rip]=%lx fr[cs]=%lx fr[rfl]=%lx "
                         "fr[rsp]=%lx retslot=%lx\r\n",
                         current_thread->tid,
                         (unsigned long)(uintptr_t)r, int_no,
                         (unsigned long)fr[15], (unsigned long)fr[17],
                         (unsigned long)fr[18], (unsigned long)fr[19],
                         (unsigned long)fr[20], (unsigned long)retslot);
        }
        current_thread->kernel_rsp = (uint64_t)r;
    }

#ifdef CONFIG_DEBUG
    if (int_no == 0 && current_thread) {
        /* Yield path: r->rip is .yield_resume; the caller's return
         * address sits at frame+176 (after 15 regs, int_no, err_code,
         * rip, cs, rflags, rsp_slot, ss).  Print it if it looks wrong. */
        uint64_t retaddr = *(uint64_t *)((char *)r + 176);
        if (retaddr < 0x100000 || retaddr > 0x116000)
            debug_printf("yield BAD tid=%u frame=%p ret=%lx\r\n",
                         current_thread->tid, (void *)r, retaddr);
    }
#endif

    if (!current_thread)
        return context_switch(r);

    if (current_thread->state != THREAD_RUNNING)
        return context_switch(r);

    return (void *)r;
}


/* Blocking + timed sleep                                              */

/* Mark the calling thread BLOCKED and remove it from the runqueue so
 * a later scheduler_unblock_thread() re-enqueues it exactly once. */
void scheduler_block_current(void) {
    thread_t *cur = current_thread;
    if (!cur || cur == idle_thread)
        return;

    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    if (cur->array)
        prio_array_dequeue(cur->array, cur, cur->prio);
    cur->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* Wake every sleeper whose deadline has passed.  Called from the timer
 * tick (IRQ context, IF off) — sched_lock is safe because a syscall-side
 * mutation always holds it with interrupts disabled. */
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

/* Block the calling thread until clockevent_get_ticks() reaches
 * deadline.  Returns 1 if the thread was blocked (the caller must
 * thread_yield() and wait for the deadline or a signal wakeup),
 * 0 if the deadline already passed (no blocking needed), -1 if it
 * could not sleep (idle thread or sleep queue full). */
int scheduler_sleep_ticks(uint64_t deadline) {
    thread_t *cur = current_thread;
    if (!cur || cur == idle_thread)
        return -1;

    if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
        return 0;

    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    if (cur->array)
        prio_array_dequeue(cur->array, cur, cur->prio);
    cur->sleep_until = (uint32_t)deadline;
    cur->state = THREAD_BLOCKED;

    int added = (sleep_count < SLEEP_MAX);
    if (added)
        sleep_queue[sleep_count++] = cur;

    spin_unlock_irqrestore(&sched_lock, flags);

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
    memset(&active_array, 0, sizeof(prio_array_t));
    memset(&expired_array, 0, sizeof(prio_array_t));
    current_thread = NULL;

    if (setup_idle() < 0)
        return;

    idle_thread->state = THREAD_RUNNING;
    current_thread = idle_thread;
    register_interrupt_handler(7, fpu_nm_handler);
    log_print(LOG_LEVEL_DEBUG, "scheduler: O(1) init done\n");
}

void scheduler_add_thread(thread_t *thread) {
    if (!thread) return;

    if (thread->static_prio == 0 && thread->tid != 0)
        thread->static_prio = DEFAULT_PRIO;
    if (thread->prio == 0)
        thread->prio = thread->static_prio;
    if (thread->time_slice == 0)
        thread->time_slice = prio_to_timeslice(thread->static_prio);

    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    thread->state = THREAD_READY;
    prio_array_enqueue(active, thread, thread->prio);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void scheduler_remove_thread(thread_t *thread) {
    if (!thread || !thread->array) return;

    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    if (thread->array) {
        prio_array_dequeue(thread->array, thread, thread->prio);
        thread->state = THREAD_ZOMBIE;
    }

    spin_unlock_irqrestore(&sched_lock, flags);
}

void scheduler_unblock_thread(thread_t *thread) {
    if (!thread) return;

    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    if (thread->state == THREAD_BLOCKED) {
        thread->state = THREAD_READY;
        if (thread->sleep_avg < MAX_SLEEP_AVG)
            thread->sleep_avg += 10;
        if (thread->sleep_avg > MAX_SLEEP_AVG)
            thread->sleep_avg = MAX_SLEEP_AVG;

        thread->prio = effective_prio(thread->static_prio, thread->sleep_avg);
        prio_array_enqueue(active, thread, thread->prio);
    }

    spin_unlock_irqrestore(&sched_lock, flags);
}

thread_t *scheduler_current_thread(void) {
    return current_thread;
}

void scheduler_set_nice(thread_t *t, int nice) {
    if (!t) return;
    if (nice < -20) nice = -20;
    if (nice > 19)  nice = 19;

    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    int new_static = nice_to_prio(nice);
    if (t->array) {
        prio_array_dequeue(t->array, t, t->prio);
        t->static_prio = new_static;
        t->prio = effective_prio(t->static_prio, t->sleep_avg);
        t->time_slice = prio_to_timeslice(t->static_prio);
        prio_array_enqueue(t->array, t, t->prio);
    } else {
        t->static_prio = new_static;
        t->prio = effective_prio(t->static_prio, t->sleep_avg);
    }

    spin_unlock_irqrestore(&sched_lock, flags);
}
