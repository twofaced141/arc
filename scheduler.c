#include "scheduler.h"
#include "process.h"
#include "gdt.h"
#include "debug.h"
#include "vmm.h"
#include "terminal.h"

static process_t *process_list[MAX_PROCESSES];
static int process_count;
static int current_index;
static process_t *idle_process;
static uint32_t ticks;

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
    frame->esp = 0;
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
    debug_printf("scheduler: idle kesp=0x%x\r\n", idle_process->kernel_esp);
    return 0;
}

void scheduler_init(void) {
    process_count = 0;
    current_index = -1;

    if (setup_idle() < 0) {
        terminal_print("SCHEDULER: failed to create idle\n");
        return;
    }
    terminal_print("SCHEDULER: idle task created\n");
}

void scheduler_add_process(process_t *proc) {
    if (process_count >= MAX_PROCESSES) return;
    process_list[process_count++] = proc;
    debug_printf("scheduler: added pid=%u count=%d\r\n", proc->pid, process_count);
}

static process_t *scheduler_pick_next(void) {
    if (process_count == 0)
        return idle_process;

    int start = current_index + 1;
    for (int i = 0; i < process_count; i++) {
        int idx = (start + i) % process_count;
        if (process_list[idx]->state == PROC_READY || process_list[idx]->state == PROC_RUNNING) {
            current_index = idx;
            return process_list[idx];
        }
    }
    return idle_process;
}

void *scheduler_switch(registers_t *r) {
    uint32_t int_no = r->int_no;

    if (int_no == 32) {
        ticks++;

        if (current_index >= 0 && current_index < process_count) {
            process_t *cur = process_list[current_index];
            if (cur && cur->state == PROC_RUNNING) {
                cur->kernel_esp = (uint32_t)r;
                if (cur->time_slice > 0)
                    cur->time_slice--;
            }
        }

        if (current_index < 0 || current_index >= process_count || process_count == 0)
            return (void *)idle_process->kernel_esp;

        process_t *cur = process_list[current_index];
        if (cur->time_slice == 0) {
            cur->state = PROC_READY;
            process_t *next = scheduler_pick_next();
            next->state = PROC_RUNNING;
            next->time_slice = 5;

            if (next == idle_process && cur->state == PROC_READY)
                next = cur;

            if (next != cur) {
                if (next->page_dir)
                    vmm_switch_directory(next->page_dir);
                tss_set_kernel_stack(next->kernel_stack_top);
                if (next->pid != 0 && cur->pid != 0)
                    debug_printf("switch %u -> %u\r\n", cur->pid, next->pid);
                return (void *)next->kernel_esp;
            }
            cur->state = PROC_RUNNING;
            cur->time_slice = 5;
        }

        return (void *)r;
    }

    if (int_no == 128) {
        switch (r->eax) {
        case 1: {
            terminal_putchar((char)r->ebx);
            return (void *)r;
        }
        case 2: {
            if (current_index >= 0 && current_index < process_count) {
                process_t *cur = process_list[current_index];
                cur->kernel_esp = (uint32_t)r;
                cur->state = PROC_READY;
                process_t *next = scheduler_pick_next();
                next->state = PROC_RUNNING;
                next->time_slice = 5;
                if (next->page_dir)
                    vmm_switch_directory(next->page_dir);
                tss_set_kernel_stack(next->kernel_stack_top);
                return (void *)next->kernel_esp;
            }
            return (void *)r;
        }
        case 3: {
            if (current_index >= 0 && current_index < process_count) {
                process_t *cur = process_list[current_index];
                process_exit(cur);
                process_t *next = scheduler_pick_next();
                next->state = PROC_RUNNING;
                next->time_slice = 5;
                if (next->page_dir)
                    vmm_switch_directory(next->page_dir);
                tss_set_kernel_stack(next->kernel_stack_top);
                return (void *)next->kernel_esp;
            }
            return (void *)r;
        }
        }
        return (void *)r;
    }

    if (current_index >= 0 && current_index < process_count) {
        process_t *cur = process_list[current_index];
        if (cur && cur->state == PROC_RUNNING)
            cur->kernel_esp = (uint32_t)r;
    }

    return (void *)r;
}
