#include "thread.h"
#include "task.h"
#include "pmm.h"
#include "scheduler.h"
#include "memory.h"
#include "uart.h"
#include "string.h"

static thread_t threads[MAX_THREADS];
static uint32_t next_tid = 1;

extern void thread_exit_trampoline(void);
__asm__(
    ".global thread_exit_trampoline\n"
    "thread_exit_trampoline:\n"
    "    b thread_exit\n"
);

void thread_init(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = THREAD_UNUSED;
        threads[i].tid = 0;
        threads[i].task = NULL;
    }
    uart_print("thread: init done\r\n");
}

thread_t *thread_create(uint64_t entry, void *page_dir, int user) {
    thread_t *thr = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            thr = &threads[i];
            break;
        }
        if (threads[i].state == THREAD_ZOMBIE && threads[i].kernel_stack) {
            pmm_free_pages(threads[i].kernel_stack, THREAD_KSTACK_SIZE / PAGE_SIZE);
            threads[i].kernel_stack = 0;
            thr = &threads[i];
            break;
        }
    }
    if (!thr) return 0;

    thr->tid = next_tid++;
    thr->state = THREAD_READY;
    thr->time_slice = 0;
    thr->static_prio = 120;
    thr->prio = 120;
    thr->sleep_avg = 50;
    thr->next = 0;
    thr->prev = 0;
    thr->array = 0;
    thr->task = NULL;
    thr->page_dir = page_dir;
    thr->entry = entry;

    const char *d = user ? "user_thread" : "kernel_thread";
    int ni = 0;
    while (d[ni] && ni < 31) { thr->name[ni] = d[ni]; ni++; }
    thr->name[ni] = 0;

    thr->kernel_stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!thr->kernel_stack) { thr->state = THREAD_UNUSED; return 0; }
    thr->kernel_stack_top = (uint64_t)thr->kernel_stack + THREAD_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(thr->kernel_stack_top - sizeof(registers_t));
    frame->far = 0;
    frame->esr = 0;
    for (int i = 0; i < 30; i++) frame->x[i] = 0;
    frame->lr = (uint64_t)thread_exit_trampoline;
    frame->elr = entry;
    if (user) {
        frame->sp = USER_STACK_TOP;
        frame->spsr = 0;
    } else {
        frame->sp = thr->kernel_stack_top;
        frame->spsr = 0x345;
    }

    thr->kernel_rsp = (uint64_t)frame;

    thread_fpu_state_init(thr->fpu_state);

    return thr;
}

thread_t *thread_current(void) {
    return scheduler_current_thread();
}

thread_t *thread_find(uint32_t tid) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].tid == tid && threads[i].state != THREAD_UNUSED)
            return &threads[i];
    }
    return 0;
}

uint32_t thread_get_tid(void) {
    thread_t *cur = thread_current();
    return cur ? cur->tid : 0;
}

extern void thread_exit_switch(uint64_t rsp) __attribute__((noreturn));

void thread_exit(int exitcode) {
    thread_t *cur = thread_current();
    if (!cur) return;
    cur->state = THREAD_ZOMBIE;
    scheduler_remove_thread(cur);

    __asm__ __volatile__("msr daifset, #2" ::: "memory");

    /* Switch to the next runnable thread instead of parking the CPU.
     * The kernel stack is reclaimed by thread_create() when the zombie
     * slot is reused — we must not free it while still running on it. */
    uint64_t next_rsp = (uint64_t)scheduler_switch((registers_t *)cur->kernel_rsp);
    thread_exit_switch(next_rsp);
    for (;;) __asm__ __volatile__("wfi");
}
