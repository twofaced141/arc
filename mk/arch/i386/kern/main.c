#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "isr.h"
#include "gdt.h"
#include "debug.h"
#include "panic.h"
#include "pit.h"
#include "clockevent.h"
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "task.h"
#include "port.h"
#include "vm_object.h"
#include "scheduler.h"
#include "pci.h"
#include "device.h"
#include "multiboot2.h"
#include "serial.h"
#include <arc/boot.h>
#include "test.h"
#include "personality.h"

/* Forward declarations from mk components */
void gdt_install(void);
void df_tss_sync_cr3(void);
void pic_remap(void);

/* Default handler for unimplemented syscalls */
static int syscall_stub(registers_t *r) {
    (void)r;
    return -1;
}

static void gp_fault_handler(registers_t *r) {
    if ((r->cs & 3) == 3) {
        log_printf(LOG_LEVEL_ERROR, "GP fault (int 13) at EIP=0x%x (err=0x%x) "
                     "in user mode — terminating process\r\n",
                     r->eip, r->err_code);
        if (proc_current && proc_exit) {
            struct proc *p = proc_current();
            if (p) {
                proc_exit(132, r);
                return;
            }
        }
    }
    panic("General Protection Fault", r);
}

/* Microkernel syscall handler — delegates BSD syscalls to bsd layer */
static void mk_syscall_handler(registers_t *r) {
    uint32_t syscall_no = r->eax;

    /* mk-level syscalls (low-numbered, kernel services) */
    switch (syscall_no) {
    case 0:
        break;
    case 1:
        thread_exit((int)r->ebx);
        return;
    case 2:
        /* Yield is automatic — scheduler_switch runs on every
         * syscall (int 128) and re-enqueues RUNNING threads. */
        return;
    case 8:
        r->eax = thread_get_tid();
        return;
    case 9:
        if (r->ebx)
            debug_putchar((char)(r->ebx & 0xFF));
        return;
    case 10:
        {
            thread_t *cur = thread_current();
            if (cur) {
                cur->sleep_until = clockevent_get_ticks() + r->ebx;
                cur->state = THREAD_BLOCKED;
            }
        }
        return;
    case 11:
        r->eax = (uint32_t)clockevent_get_ticks();
        return;
    case 12:
        r->eax = (uint32_t)-1;  /* shutdown not supported on i386 */
        return;

    /* ---- Phase 1: Task + C-space ---- */
    case 13:
        r->eax = (uint32_t)sys_task_create();
        return;
    case 14:
        r->eax = (uint32_t)sys_task_destroy();
        return;
    case 15:
        r->eax = (uint32_t)sys_slot_alloc(r->ebx, r->ecx);
        return;
    case 16:
        r->eax = (uint32_t)sys_slot_free(((uint64_t)r->edx << 32) | r->ebx);
        return;

    /* ---- Phase 1: Ports ---- */
    case 17:
        r->eax = (uint32_t)sys_port_create();
        return;
    case 18:
        r->eax = (uint32_t)sys_port_destroy(((uint64_t)r->edx << 32) | r->ebx);
        return;
    case 19:
        r->eax = (uint32_t)sys_port_send(((uint64_t)r->edx << 32) | r->ebx,
                                          (const ipc_msg_t *)r->ecx);
        return;
    case 20:
        r->eax = (uint32_t)sys_port_recv(((uint64_t)r->edx << 32) | r->ebx,
                                          (ipc_msg_t *)r->ecx);
        return;
    case 21:
        r->eax = (uint32_t)sys_port_call(((uint64_t)r->edx << 32) | r->ebx,
                                          (ipc_msg_t *)r->ecx);
        return;
    case 22:
        r->eax = (uint32_t)sys_port_reply(((uint64_t)r->edx << 32) | r->ebx,
                                           (const ipc_msg_t *)r->ecx);
        return;
    case 23:
        r->eax = (uint32_t)sys_port_notify(((uint64_t)r->edx << 32) | r->ebx);
        return;
    case 24:
        r->eax = (uint32_t)sys_port_poll((const uint64_t *)r->ebx,
                                          (int)r->ecx, ((uint64_t)r->esi << 32) | r->edx);
        return;

    /* ---- Phase 3: VM operations ---- */
    case 25:
        r->eax = (uint32_t)sys_vm_create_shared(((uint64_t)r->edx << 32) | r->ebx);
        return;
    case 26:
        r->eax = (uint32_t)sys_vm_create_phys(
                     ((uint64_t)r->edx << 32) | r->ebx,
                     ((uint64_t)r->esi << 32) | r->ecx);
        return;
    case 27:
        r->eax = (uint32_t)sys_vm_map(
                     ((uint64_t)r->edx << 32) | r->ebx,
                     ((uint64_t)r->esi << 32) | r->ecx,
                     r->edi);
        return;
    case 28:
        r->eax = (uint32_t)sys_vm_unmap(
                     ((uint64_t)r->edx << 32) | r->ebx,
                     ((uint64_t)r->esi << 32) | r->ecx);
        return;
    case 29:
        r->eax = (uint32_t)sys_vm_protect(
                     ((uint64_t)r->edx << 32) | r->ebx,
                     ((uint64_t)r->esi << 32) | r->ecx,
                     r->edi);
        return;
    }

    /* BSD syscalls (>= 1024) or delegate by flag */
    if (syscall_no >= 1024) {
        if (bsd_syscall_dispatch)
            r->eax = (uint32_t)bsd_syscall_dispatch(r);
        else
            r->eax = (uint32_t)syscall_stub(r);
        return;
    }

    /* Fallback */
    r->eax = (uint32_t)syscall_stub(r);
}

/* Initialize microkernel subsystems */
void mk_init(struct arc_boot_info *boot_info) {
    log_print(LOG_LEVEL_DEBUG, "mk: init started\r\n");

    /* Interrupt infrastructure */
    idt_init();
    gdt_install();
    pic_remap();

    /* Set up IDT gates for ISRs 0-31 */
    extern void isr0(void); extern void isr1(void); extern void isr2(void);
    extern void isr3(void); extern void isr4(void); extern void isr5(void);
    extern void isr6(void); extern void isr7(void); extern void isr8(void);
    extern void isr9(void); extern void isr10(void); extern void isr11(void);
    extern void isr12(void); extern void isr13(void); extern void isr14(void);
    extern void isr15(void); extern void isr16(void); extern void isr17(void);
    extern void isr18(void); extern void isr19(void); extern void isr20(void);
    extern void isr21(void); extern void isr22(void); extern void isr23(void);
    extern void isr24(void); extern void isr25(void); extern void isr26(void);
    extern void isr27(void); extern void isr28(void); extern void isr29(void);
    extern void isr30(void); extern void isr31(void); extern void isr128(void);
    extern void irq0(void); extern void irq1(void); extern void irq2(void);
    extern void irq3(void); extern void irq4(void); extern void irq5(void);
    extern void irq6(void); extern void irq7(void); extern void irq8(void);
    extern void irq9(void); extern void irq10(void); extern void irq11(void);
    extern void irq12(void); extern void irq13(void); extern void irq14(void);
    extern void irq15(void);

    uint16_t code_sel = 0x08;
    idt_set_gate(0, (uint32_t)isr0, code_sel, 0x8E);
    idt_set_gate(1, (uint32_t)isr1, code_sel, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, code_sel, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, code_sel, 0x8E);
    idt_set_gate(4, (uint32_t)isr4, code_sel, 0x8E);
    idt_set_gate(5, (uint32_t)isr5, code_sel, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, code_sel, 0x8E);
    idt_set_gate(7, (uint32_t)isr7, code_sel, 0x8E);
    /* Double fault as a task gate: hardware task-switches to the dedicated
     * df_tss (fresh stack, never busy) instead of pushing onto a possibly
     * broken stack — the 32-bit analogue of the 64-bit IST. */
    idt_set_gate(8, 0, DF_TSS_SEL, 0x85);
    idt_set_gate(9, (uint32_t)isr9, code_sel, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, code_sel, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, code_sel, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, code_sel, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, code_sel, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, code_sel, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, code_sel, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, code_sel, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, code_sel, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, code_sel, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, code_sel, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, code_sel, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, code_sel, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, code_sel, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, code_sel, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, code_sel, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, code_sel, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, code_sel, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, code_sel, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, code_sel, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, code_sel, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, code_sel, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, code_sel, 0x8E);
    idt_set_gate(32, (uint32_t)irq0, code_sel, 0x8E);
    idt_set_gate(33, (uint32_t)irq1, code_sel, 0x8E);
    idt_set_gate(34, (uint32_t)irq2, code_sel, 0x8E);
    idt_set_gate(35, (uint32_t)irq3, code_sel, 0x8E);
    idt_set_gate(36, (uint32_t)irq4, code_sel, 0x8E);
    idt_set_gate(37, (uint32_t)irq5, code_sel, 0x8E);
    idt_set_gate(38, (uint32_t)irq6, code_sel, 0x8E);
    idt_set_gate(39, (uint32_t)irq7, code_sel, 0x8E);
    idt_set_gate(40, (uint32_t)irq8, code_sel, 0x8E);
    idt_set_gate(41, (uint32_t)irq9, code_sel, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, code_sel, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, code_sel, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, code_sel, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, code_sel, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, code_sel, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, code_sel, 0x8E);
    /* Syscall gate DPL=3 for userspace */
    idt_set_gate(128, (uint32_t)isr128, code_sel, 0xEE);

    register_interrupt_handler(8,   double_fault_handler);
    register_interrupt_handler(13,  gp_fault_handler);
    register_interrupt_handler(128, mk_syscall_handler);
    log_print(LOG_LEVEL_DEBUG, "mk: IDT/PIC done\r\n");

    /* Memory management */
    if (boot_info) {
        multiboot2_info_t *raw = (multiboot2_info_t *)arc_boot_raw_info();
        pmm_init(raw);
        {
            uint32_t cr0;
            __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
            cr0 |= (1 << 1) | (1 << 5);  /* Monitor co-proc + NE */
            cr0 &= ~(1 << 2);             /* Clear EM (FPU present) */
            __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
        }
        {
            uint32_t cr4;
            __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1 << 9) | (1 << 10);  /* OSFXSR + OSXMMEXCPT */
            __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
        }
        vmm_init();
        vmm_init_heap();
        vm_object_init();
        vm_object_test_shared();
        /* The double-fault task switch loads CR3 from df_tss: point it at
         * the now-active kernel page directory. */
        df_tss_sync_cr3();
        log_print(LOG_LEVEL_DEBUG, "mk: VM done\r\n");
    }

    /* Task, threading and scheduling */
    task_init();
    thread_init();
    scheduler_init();
    log_print(LOG_LEVEL_DEBUG, "mk: threads done\r\n");

    /* Timer */
    pit_init();
    pci_init();
    i8042_init();
    log_print(LOG_LEVEL_DEBUG, "mk: init done\r\n");
}

void kernel_main(struct arc_boot_info *boot);

void _entry(uint32_t mboot_magic, void *mboot_info) {
    debug_init();
    serial_init();
    log_print(LOG_LEVEL_INFO, "arc kernel i386\n");

    struct arc_boot_info *bi = arc_boot_init(mboot_magic, mboot_info);
    if (arc_boot_validate(bi) != 0) {
        log_print(LOG_LEVEL_ERROR, "boot: validation failed\n");
    }
    log_parse_cmdline((bi && (bi->flags & ARC_BOOT_HAS_CMDLINE)) ? bi->cmdline : NULL);
    arc_boot_dump(bi);
    kernel_main(bi);
}

void kernel_main(struct arc_boot_info *boot) {
    (void)boot;

    /* Initialize microkernel first */
    mk_init(boot);

    /* Create the kernel task that owns kernel threads */
    task_t *kernel_task = task_create("kernel");
    if (kernel_task) {
        log_printf(LOG_LEVEL_DEBUG, "kernel_task: id=%u\n", kernel_task->task_id);
    }

    /* Initialize BSD personality layer */
    // test_run_boot_tests();

    if (bsd_init) {
        bsd_init(boot->cmdline);
        log_print(LOG_LEVEL_INFO, "arc: BSD layer initialized\n");
    } else {
        log_print(LOG_LEVEL_WARN, "arc: no BSD layer — standalone microkernel\n");
    }

    /* Create the boot-time kernel pager thread */
    boot_pager_init();

    /* Enable interrupts — scheduler starts here */
    __asm__ __volatile__("sti");
    log_print(LOG_LEVEL_DEBUG, "arc: interrupts enabled, entering idle\n");

    /* Idle loop — kernel never returns */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
