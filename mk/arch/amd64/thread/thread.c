#include "thread.h"
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "debug.h"
#include "scheduler.h"
#include "gdt.h"

static thread_t threads[MAX_THREADS];
static uint32_t next_tid = 1;
spinlock_t thread_lock = SPINLOCK_INIT;

void thread_init(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = THREAD_UNUSED;
        threads[i].page_dir = NULL;
        threads[i].tid = 0;
        threads[i].task = NULL;
    }
    log_print(LOG_LEVEL_DEBUG, "thread: init done\r\n");
}

thread_t *thread_create(uint64_t rip, page_directory_t *page_dir, int user) {
    thread_t *thr = NULL;
    uint32_t flags;
    spin_lock_irqsave(&thread_lock, &flags);

    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            thr = &threads[i];
            break;
        }
        if (threads[i].state == THREAD_ZOMBIE && threads[i].kernel_stack) {
            pmm_free_pages(threads[i].kernel_stack, THREAD_KSTACK_SIZE / PAGE_SIZE);
            threads[i].kernel_stack = NULL;
            thr = &threads[i];
            break;
        }
    }
    if (!thr) { spin_unlock_irqrestore(&thread_lock, flags); return NULL; }

    thr->tid = next_tid++;
    thr->state = THREAD_READY;
    thr->time_slice = 0;
    thr->sleep_until = 0;
    thr->static_prio = 120;
    thr->prio = 120;
    thr->sleep_avg = 50;
    thr->next = NULL;
    thr->prev = NULL;
    thr->array = NULL;
    thr->task = NULL;

    {
        int ni = 0;
        const char *d = user ? "user_thread" : "kernel_thread";
        while (d[ni] && ni < 31) { thr->name[ni] = d[ni]; ni++; }
        thr->name[ni] = '\0';
    }

    spin_unlock_irqrestore(&thread_lock, flags);

    thr->kernel_stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!thr->kernel_stack) { thr->state = THREAD_UNUSED; return NULL; }
    thr->kernel_stack_top = (uint64_t)thr->kernel_stack + THREAD_KSTACK_SIZE;

    log_printf(LOG_LEVEL_DEBUG, "thread: tid=%u '%s' stack=%p top=0x%lx size=%u\r\n",
                 thr->tid, thr->name, (void *)thr->kernel_stack,
                 thr->kernel_stack_top, THREAD_KSTACK_SIZE);

    thr->page_dir = page_dir;

    registers_t *frame = (registers_t *)(thr->kernel_stack_top - sizeof(registers_t));
    frame->rax = 0; frame->rbx = 0; frame->rcx = 0; frame->rdx = 0;
    frame->rsi = 0; frame->rdi = 0; frame->rbp = 0;
    frame->r8 = 0; frame->r9 = 0; frame->r10 = 0; frame->r11 = 0;
    frame->r12 = 0; frame->r13 = 0; frame->r14 = 0; frame->r15 = 0;
    frame->int_no = 0; frame->err_code = 0;
    frame->rip = rip;
    frame->cs = user ? 0x2B : 0x08;
    frame->rflags = 0x202;
    frame->rsp = user ? USER_STACK_TOP : thr->kernel_stack_top;
    frame->ss = user ? 0x23 : 0x10;

    log_printf(LOG_LEVEL_DEBUG, "thread: tid=%u frame=0x%lx rip=0x%lx cs=0x%lx "
                 "rsp_slot=0x%lx ss=0x%lx\r\n",
                 thr->tid, (uint64_t)(uintptr_t)frame, frame->rip,
                 frame->cs, frame->rsp, frame->ss);

    thr->kernel_rsp = (uint64_t)frame;
    thr->rip = rip;
    thr->user_rsp = frame->rsp;

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
    return NULL;
}

uint32_t thread_get_tid(void) {
    thread_t *cur = thread_current();
    return cur ? cur->tid : 0;
}

extern void thread_exit_switch(uint64_t rsp) __attribute__((noreturn));

void thread_exit(int exitcode) {
    thread_t *cur = thread_current();
    if (!cur) return;
    log_printf(LOG_LEVEL_DEBUG, "thread: tid=%u exit(%d)\r\n", cur->tid, exitcode);

    uint32_t flags;
    spin_lock_irqsave(&thread_lock, &flags);
    cur->state = THREAD_ZOMBIE;
    spin_unlock_irqrestore(&thread_lock, flags);

    scheduler_remove_thread(cur);

    __asm__ __volatile__("cli");

    /* Switch to the next runnable thread instead of parking the CPU.
     * The kernel stack is reclaimed by thread_create() when the zombie
     * slot is reused — we must not free it while still running on it. */
    uint64_t next_rsp = (uint64_t)scheduler_switch((registers_t *)cur->kernel_rsp);
    thread_exit_switch(next_rsp);
    for (;;) { __asm__ __volatile__("hlt"); }
}
