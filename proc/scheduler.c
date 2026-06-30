#include "scheduler.h"
#include "process.h"
#include "gdt.h"
#include "debug.h"
#include "vmm.h"
#include "terminal.h"
#include "spinlock.h"
#include "signal.h"

static process_t *process_list[MAX_PROCESSES];
static int process_count;
static int current_index;
static process_t *idle_process;
static uint32_t ticks;
static spinlock_t sched_lock = SPINLOCK_INIT;

uint32_t scheduler_syscall_no;
uint32_t scheduler_signal_pending;

static void idle_entry(void) {
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

static int setup_idle(void) {
    idle_process = (process_t *)kmalloc(sizeof(process_t));
    if (!idle_process) return -1;

    idle_process->pid = 0;
    idle_process->state = PROC_READY;
    idle_process->time_slice = 1;
    idle_process->page_dir = NULL;  // use kernel page dir
    idle_process->kernel_stack = (uint8_t *)pmm_alloc_pages(PROC_KSTACK_SIZE / PAGE_SIZE);
    if (!idle_process->kernel_stack) return -1;
    idle_process->kernel_stack_top = (uint32_t)idle_process->kernel_stack + PROC_KSTACK_SIZE;

    registers_t *frame = (registers_t *)(idle_process->kernel_stack_top - sizeof(registers_t));
    frame->gs = 0x10;
    frame->fs = 0x10;
    frame->es = 0x10;
    frame->ds = 0x10;
    frame->edi = 0;
    frame->esi = 0;
    frame->ebp = 0;
    frame->esp = (uint32_t)&frame->int_no;
    frame->ebx = 0;
    frame->edx = 0;
    frame->ecx = 0;
    frame->eax = 0;
    frame->int_no = 0;
    frame->err_code = 0;
    frame->eip = (uint32_t)idle_entry;
    frame->cs = 0x08;
    frame->eflags = 0x202;
    frame->useresp = 0;
    frame->ss = 0x10;

    idle_process->kernel_esp = (uint32_t)frame;
    return 0;
}

void scheduler_init(void) {
    process_count = 0;
    current_index = -1;

    if (setup_idle() < 0)
        return;
}

void scheduler_add_process(process_t *proc) {
    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    if (process_count >= MAX_PROCESSES) { spin_unlock_irqrestore(&sched_lock, flags); return; }
    process_list[process_count++] = proc;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void scheduler_remove_process(process_t *proc) {
    uint32_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    for (int i = 0; i < process_count; i++) {
        if (process_list[i] != proc) continue;
        if (current_index == i)
            current_index = -1;
        else if (i < current_index)
            current_index--;
        process_list[i] = process_list[--process_count];
        if (current_index >= process_count)
            current_index = -1;
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}


static void *context_switch(registers_t *r) {
    int start = (current_index >= 0) ? current_index + 1 : 0;
    if (start >= process_count) start = 0;
    for (int i = 0; i < process_count; i++) {
        int idx = (start + i) % process_count;
        process_t *p = process_list[idx];
        if (p->state == PROC_READY || p->state == PROC_RUNNING) {
            if (idx == current_index)
                return (void *)r;
            current_index = idx;
            p->state = PROC_RUNNING;
            p->time_slice = 5;
            if (p->page_dir)
                vmm_switch_directory(p->page_dir);
            tss_set_kernel_stack(p->kernel_stack_top);
            return (void *)p->kernel_esp;
        }
    }
    current_index = -1;
    return (void *)r;
}

void *scheduler_switch(registers_t *r) {
    uint32_t int_no = r->int_no;

    if (int_no == 32) {
        ticks++;

        for (int i = 0; i < process_count; i++) {
            process_t *p = process_list[i];
            if (p->state == PROC_BLOCKED && p->sleep_until && p->sleep_until <= ticks) {
                p->state = PROC_READY;
                p->sleep_until = 0;
            }
        }

        if (current_index >= 0 && current_index < process_count) {
            process_t *cur = process_list[current_index];
            scheduler_check_signals(r);
            if (cur->state == PROC_RUNNING) {
                cur->kernel_esp = (uint32_t)r;
                if (cur->time_slice > 0)
                    cur->time_slice--;
                if (cur->time_slice == 0)
                    cur->state = PROC_READY;
            }
        }

        if (process_count == 0)
            return (void *)r;

        return context_switch(r);
    }

    if (int_no == 128) {
        uint32_t flags;
        spin_lock_irqsave(&sched_lock, &flags);
        switch (scheduler_syscall_no) {
        case 2:
            if (current_index >= 0 && current_index < process_count) {
                process_t *cur = process_list[current_index];
                scheduler_check_signals(r);
                cur->kernel_esp = (uint32_t)r;
                cur->state = PROC_READY;
            }
            {
                void *next = context_switch(r);
                spin_unlock_irqrestore(&sched_lock, flags);
                return next;
            }
        case 3: {
            scheduler_check_signals(r);
            void *next = context_switch(r);
            if (next == (void *)r)
                next = (void *)idle_process->kernel_esp;
            spin_unlock_irqrestore(&sched_lock, flags);
            return next;
        }
        case 6:
        case 16:
            {
                scheduler_check_signals(r);
                void *next = context_switch(r);
                spin_unlock_irqrestore(&sched_lock, flags);
                return next;
            }
        }
        scheduler_check_signals(r);
        spin_unlock_irqrestore(&sched_lock, flags);
        return (void *)r;
    }

    if (current_index >= 0 && current_index < process_count) {
        process_t *cur = process_list[current_index];
        if (cur->state == PROC_RUNNING)
            cur->kernel_esp = (uint32_t)r;
    }

    if (current_index < 0 && process_count > 0)
        return context_switch(r);
    if (current_index < 0)
        return (void *)idle_process->kernel_esp;

    scheduler_check_signals(r);
    return (void *)r;
}

void scheduler_check_signals(registers_t *r) {
    if (!scheduler_signal_pending) return;
    process_t *proc = scheduler_current_process();
    if (!proc) return;
    if (r->cs != 0x1B) return;
    scheduler_signal_pending = 0;
    deliver_signal(r);
}

process_t *scheduler_current_process(void) {
    if (current_index >= 0 && current_index < process_count)
        return process_list[current_index];
    return NULL;
}
