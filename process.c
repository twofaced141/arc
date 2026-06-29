#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "terminal.h"
#include "debug.h"
#include "isr.h"
#include "gdt.h"

static process_t processes[MAX_PROCESSES];
static uint32_t next_pid = 1;

void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].pid = 0;
        processes[i].state = PROC_UNUSED;
        processes[i].page_dir = NULL;
    }
    debug_print("process: init done\r\n");
}

process_t *process_create_user(uint32_t eip, const void *code, uint32_t code_size) {
    process_t *proc = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            proc = &processes[i];
            break;
        }
    }
    if (!proc) return NULL;

    proc->pid = next_pid++;
    proc->state = PROC_READY;
    proc->time_slice = 5;

    proc->kernel_stack = (uint8_t *)pmm_alloc_pages(PROC_KSTACK_SIZE / PAGE_SIZE);
    if (!proc->kernel_stack) return NULL;
    proc->kernel_stack_top = (uint32_t)proc->kernel_stack + PROC_KSTACK_SIZE;

    proc->page_dir = vmm_create_directory();
    if (!proc->page_dir) return NULL;

    uint32_t pages = (code_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t code_virt = eip & ~0xFFF;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) return NULL;
        if (i == 0) {
            uint8_t *tmp = (uint8_t *)vmm_temp_map(phys);
            for (uint32_t j = 0; j < code_size && j < PAGE_SIZE; j++)
                tmp[j] = ((uint8_t *)code)[j];
            vmm_temp_unmap();
        }
        vmm_map_page(proc->page_dir, phys, code_virt + i * PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }

    uint32_t user_stack_top = 0xC0000000;
    uint32_t user_stack_phys = (uint32_t)pmm_alloc_page();
    if (!user_stack_phys) return NULL;
    vmm_map_page(proc->page_dir, user_stack_phys, user_stack_top - PAGE_SIZE,
                 VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    proc->user_esp = user_stack_top;
    proc->eip = eip;

    registers_t *frame = (registers_t *)(proc->kernel_stack_top - sizeof(registers_t));
    frame->gs = 0x23;
    frame->fs = 0x23;
    frame->es = 0x23;
    frame->ds = 0x23;
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
    frame->eip = eip;
    frame->cs = 0x1B;
    frame->eflags = 0x202;
    frame->useresp = proc->user_esp;
    frame->ss = 0x23;

    proc->kernel_esp = (uint32_t)frame;

    debug_printf("process: created pid=%u eip=0x%x kesp=0x%x\r\n",
                 proc->pid, eip, proc->kernel_esp);
    return proc;
}

void process_exit(process_t *proc) {
    if (!proc) return;
    debug_printf("process: exit pid=%u\r\n", proc->pid);
    proc->state = PROC_UNUSED;
}
