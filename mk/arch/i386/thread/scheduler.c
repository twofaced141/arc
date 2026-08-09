#include "scheduler.h"
#include "thread.h"
#include "gdt.h"
#include "debug.h"
#include "string.h"
#include "vmm.h"
#include "spinlock.h"
#include "clockevent.h"

static prio_array_t active_array;
static prio_array_t expired_array;
static prio_array_t *active  = &active_array;
static prio_array_t *expired = &expired_array;
static thread_t    *current_thread;
static thread_t    *idle_thread;
static uint32_t ticks;
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Sleeping threads (see amd64 scheduler for the design). */
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
    __asm__ __volatile__("fxsave (%0)" :: "r"(t->fpu_state) : "memory");
}

static inline void fpu_restore(thread_t *t) {
    __asm__ __volatile__("fxrstor (%0)" :: "r"(t->fpu_state) : "memory");
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
    idle_thread->kernel_stack_top = (uint32_t)idle_thread->kernel_stack + THREAD_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(idle_thread->kernel_stack_top - sizeof(registers_t));
    frame->gs = 0x10; frame->fs = 0x10; frame->es = 0x10; frame->ds = 0x10;
    frame->edi = 0; frame->esi = 0; frame->ebp = 0;
    frame->esp = (uint32_t)&frame->int_no;
    frame->ebx = 0; frame->edx = 0; frame->ecx = 0; frame->eax = 0;
    frame->int_no = 0; frame->err_code = 0;
    frame->eip = (uint32_t)idle_entry;
    frame->cs = 0x08;
    frame->eflags = 0x202;
    frame->useresp = 0;
    frame->ss = 0x10;

    idle_thread->kernel_esp = (uint32_t)frame;
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
        current_thread = idle_thread;
        return (void *)idle_thread->kernel_esp;
    }

    thread_t *next = active->queue[top_prio];
    if (next->next == next) {
        /* Single thread — actually remove it */
        active->queue[top_prio] = NULL;
        active->bitmap[top_prio / 32] &= ~(1u << (top_prio % 32));
        next->array = NULL;
        active->nr_active--;
    } else {
        /* Multiple threads — rotate, don't remove. nr_active unchanged. */
        next->prev->next = next->next;
        next->next->prev = next->prev;
        active->queue[top_prio] = next->next;
        thread_t *tail = next->next->prev;
        next->prev = tail;
        tail->next = next;
        next->next->prev = next;
        next->array = NULL;
    }

    if (next->state == THREAD_ZOMBIE || next->state == THREAD_UNUSED)
        return context_switch(r);

    next->state = THREAD_RUNNING;
    current_thread = next;

    {
        uint32_t cr0;
        __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= (1 << 3);
        __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
    }
    if (next->page_dir)
        vmm_switch_directory(next->page_dir);
    tss_set_kernel_stack(next->kernel_stack_top);
    return (void *)next->kernel_esp;
}

void *scheduler_switch(registers_t *r) {
    uint32_t int_no = r->int_no;

    if (int_no == 32) {
        ticks++;
        sleep_wake_tick();

        if (current_thread && current_thread->tid != 0 && current_thread->state == THREAD_RUNNING) {
            current_thread->kernel_esp = (uint32_t)r;

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
                current_thread = NULL;
            }
        }

        return context_switch(r);
    }

    if (int_no == 128) {
        uint32_t flags;
        spin_lock_irqsave(&sched_lock, &flags);

        if (current_thread && current_thread->tid != 0) {
            current_thread->kernel_esp = (uint32_t)r;

            /* Only re-enqueue if the syscall didn't already set a
             * special state (e.g. THREAD_BLOCKED by port_recv). */
            if (current_thread->state == THREAD_RUNNING) {
                current_thread->state = THREAD_READY;

                if (current_thread->sleep_avg > 0)
                    current_thread->sleep_avg--;

                current_thread->prio = effective_prio(current_thread->static_prio,
                                                      current_thread->sleep_avg);
                prio_array_enqueue(expired, current_thread, current_thread->prio);
                current_thread = NULL;
            }
        }

        void *next = context_switch(r);
        spin_unlock_irqrestore(&sched_lock, flags);
        return next;
    }

    if (current_thread)
        current_thread->kernel_esp = (uint32_t)r;

    if (!current_thread)
        return context_switch(r);

    if (current_thread->state != THREAD_RUNNING)
        return context_switch(r);

    return (void *)r;
}


/* Blocking + timed sleep                                              */
/* ------------------------------------------------------------------ */

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

    thread_yield();
    return 0;
}

void scheduler_init(void) {
    memset(&active_array, 0, sizeof(prio_array_t));
    memset(&expired_array, 0, sizeof(prio_array_t));
    current_thread = NULL;

    if (setup_idle() < 0)
        return;

    idle_thread->state = THREAD_RUNNING;
    current_thread = idle_thread;
    register_interrupt_handler(7, fpu_nm_handler);
    debug_print("scheduler: O(1) init done\n");
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
