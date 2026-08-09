#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "isr.h"
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
#include "acpi.h"
#include "apic.h"
#include "device.h"
#include "multiboot2.h"
#include "serial.h"
#include "string.h"
#include <arc/boot.h>
#include "test.h"
#include "personality.h"
#include "cpu.h"

void gdt_install(void);
void pic_remap(void);
void syscall_entry(void);
extern uint64_t syscall_kernel_rsp;

static void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}



static int syscall_stub(registers_t *r) {
    (void)r;
    return -1;
}

static void gp_fault_handler(registers_t *r) {
    if ((r->cs & 3) == 3) {
        log_printf(LOG_LEVEL_ERROR, "GP fault (int 13) at RIP=0x%lx (err=0x%lx) "
                     "in user mode — terminating process\r\n",
                     r->rip, r->err_code);
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

static void mk_syscall_handler(registers_t *r) {
    uint64_t syscall_no = r->rax;

    switch (syscall_no) {
    case 0:
        break;
    case 1:
        thread_exit((int)r->rdi);
        return;
    case 2:
        r->rax = 0;
        return;
    case 8:
        r->rax = thread_get_tid();
        return;
    case 9:
        if (r->rdi)
            debug_putchar((char)(r->rdi & 0xFF));
        return;
    case 10: {
        uint32_t deadline = (uint32_t)clockevent_get_ticks() + (uint32_t)r->rdi;
        __asm__ __volatile__("sti");
        while (clockevent_get_ticks() < deadline)
            __asm__ __volatile__("hlt");
        r->rax = 0;
        return;
    }
    case 11:
        r->rax = (uint32_t)clockevent_get_ticks();
        return;
    case 12:
        acpi_shutdown();
        return;

    /* ---- Phase 1: Task + C-space ---- */
    case 13:
        r->rax = (uint64_t)sys_task_create();
        return;
    case 14:
        r->rax = (uint64_t)sys_task_destroy();
        return;
    case 15:
        r->rax = (uint64_t)sys_slot_alloc((uint32_t)r->rdi, (uint32_t)r->rsi);
        return;
    case 16:
        r->rax = (uint64_t)sys_slot_free(r->rdi);
        return;

    /* ---- Phase 1: Ports ---- */
    case 17:
        r->rax = (uint64_t)sys_port_create();
        return;
    case 18:
        r->rax = (uint64_t)sys_port_destroy(r->rdi);
        return;
    case 19:
        r->rax = (uint64_t)sys_port_send(r->rdi, (const ipc_msg_t *)r->rsi);
        return;
    case 20:
        r->rax = (uint64_t)sys_port_recv(r->rdi, (ipc_msg_t *)r->rsi);
        return;
    case 21:
        r->rax = (uint64_t)sys_port_call(r->rdi, (ipc_msg_t *)r->rsi);
        return;
    case 22:
        r->rax = (uint64_t)sys_port_reply(r->rdi, (const ipc_msg_t *)r->rsi);
        return;
    case 23:
        r->rax = (uint64_t)sys_port_notify(r->rdi);
        return;
	    case 24:
	        r->rax = (uint64_t)sys_port_poll((const uint64_t *)r->rdi,
	                                         (int)r->rsi, r->rdx);
	        return;

	    /* ---- Phase 3: VM operations ---- */
	    case 25:
	        r->rax = (uint64_t)sys_vm_create_shared(r->rdi);
	        return;
	    case 26:
	        r->rax = (uint64_t)sys_vm_create_phys(r->rdi, r->rsi);
	        return;
	    case 27:
	        r->rax = (uint64_t)sys_vm_map(r->rdi, r->rsi, (uint32_t)r->rdx);
	        return;
	    case 28:
	        r->rax = (uint64_t)sys_vm_unmap(r->rdi, r->rsi);
	        return;
	    case 29:
	        r->rax = (uint64_t)sys_vm_protect(r->rdi, r->rsi, (uint32_t)r->rdx);
	        return;
	    }

	    if (syscall_no >= 1024) {
        if (bsd_syscall_dispatch)
            r->rax = (uint64_t)bsd_syscall_dispatch(r);
        else
            r->rax = (uint64_t)syscall_stub(r);
        return;
    }

    r->rax = (uint64_t)syscall_stub(r);
}

void mk_init(struct arc_boot_info *boot_info) {
    log_print(LOG_LEVEL_DEBUG, "mk: init started\r\n");

    idt_init();
    gdt_install();
    pic_remap();

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
    idt_set_gate(0, (uint64_t)isr0, code_sel, 0x8E);
    idt_set_gate(1, (uint64_t)isr1, code_sel, 0x8E);
    idt_set_gate_ist(2, (uint64_t)isr2, code_sel, 0x8E, 2);   /* NMI on IST2 */
    idt_set_gate(3, (uint64_t)isr3, code_sel, 0x8E);
    idt_set_gate(4, (uint64_t)isr4, code_sel, 0x8E);
    idt_set_gate(5, (uint64_t)isr5, code_sel, 0x8E);
    idt_set_gate(6, (uint64_t)isr6, code_sel, 0x8E);
    idt_set_gate(7, (uint64_t)isr7, code_sel, 0x8E);
    idt_set_gate_ist(8, (uint64_t)isr8, code_sel, 0x8E, 1);   /* Double Fault on IST1 */
    idt_set_gate(9, (uint64_t)isr9, code_sel, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, code_sel, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, code_sel, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, code_sel, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, code_sel, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, code_sel, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, code_sel, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, code_sel, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, code_sel, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, code_sel, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, code_sel, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, code_sel, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, code_sel, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, code_sel, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, code_sel, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, code_sel, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, code_sel, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, code_sel, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, code_sel, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, code_sel, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, code_sel, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, code_sel, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, code_sel, 0x8E);
    idt_set_gate(32, (uint64_t)irq0, code_sel, 0x8E);
    idt_set_gate(33, (uint64_t)irq1, code_sel, 0x8E);
    idt_set_gate(34, (uint64_t)irq2, code_sel, 0x8E);
    idt_set_gate(35, (uint64_t)irq3, code_sel, 0x8E);
    idt_set_gate(36, (uint64_t)irq4, code_sel, 0x8E);
    idt_set_gate(37, (uint64_t)irq5, code_sel, 0x8E);
    idt_set_gate(38, (uint64_t)irq6, code_sel, 0x8E);
    idt_set_gate(39, (uint64_t)irq7, code_sel, 0x8E);
    idt_set_gate(40, (uint64_t)irq8, code_sel, 0x8E);
    idt_set_gate(41, (uint64_t)irq9, code_sel, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, code_sel, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, code_sel, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, code_sel, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, code_sel, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, code_sel, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, code_sel, 0x8E);
    idt_set_gate(128, (uint64_t)isr128, code_sel, 0xEE);

    register_interrupt_handler(8,   double_fault_handler);
    register_interrupt_handler(13,  gp_fault_handler);
    register_interrupt_handler(128, mk_syscall_handler);
    log_print(LOG_LEVEL_DEBUG, "mk: IDT/PIC done\r\n");

    if (boot_info) {
        multiboot2_info_t *raw = (multiboot2_info_t *)arc_boot_raw_info();
        pmm_init(raw);
        {
            uint64_t cr0;
            __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
            cr0 |= (1 << 1) | (1 << 5) | (1 << 16);
            cr0 &= ~(1 << 2);
            __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
        }
        {
            uint64_t cr4;
            __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1 << 9) | (1 << 10);
            __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
        }
        vmm_init();
        vmm_init_heap();
        vm_object_init();
        vm_object_test_shared();  /* exercise shared memory page cache */
        log_print(LOG_LEVEL_DEBUG, "mk: VM done\r\n");
    }

    task_init();
    thread_init();
    scheduler_init();
    log_print(LOG_LEVEL_DEBUG, "mk: threads done\r\n");

    pit_init();
    pci_init();
    i8042_init();
    acpi_init(boot_info);

    /* Initialize APIC: LAPIC + I/O APIC, switch from PIC */
    if (apic_init() == 0) {
        extern int apic_enabled;
        apic_enabled = 1;
    }

    /* SMP: IPI vector + CPU discovery + per-CPU access for the BSP */
    ipi_init();
    cpu_init();

    /* Setup syscall/sysret MSRs for user mode entry */
    {
        uint64_t rsp;
        __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
        syscall_kernel_rsp = rsp;
    }
    wrmsr(0xC0000080, 1);                                              /* EFER.SCE = 1 (enable syscall) */
    wrmsr(0xC0000081, (uint64_t)0x1B << 48 | (uint64_t)0x08 << 32);  /* STAR */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);                       /* LSTAR */
    wrmsr(0xC0000083, 0x200);                                         /* SF_MASK (clear IF) */

    log_print(LOG_LEVEL_DEBUG, "mk: init done\r\n");
}

void kernel_main(struct arc_boot_info *boot);

void _entry(uint64_t mboot_magic, void *mboot_info) {
    debug_init();
    serial_init();

    log_print(LOG_LEVEL_INFO, "arc kernel amd64\n");

    struct arc_boot_info *bi = arc_boot_init((uint32_t)mboot_magic, mboot_info);
    if (arc_boot_validate(bi) != 0) {
        log_print(LOG_LEVEL_ERROR, "boot: validation failed\n");
    }
    log_parse_cmdline((bi && (bi->flags & ARC_BOOT_HAS_CMDLINE)) ? bi->cmdline : NULL);
    arc_boot_dump(bi);
    kernel_main(bi);
}

void kernel_main(struct arc_boot_info *boot) {
    (void)boot;

    mk_init(boot);

    /* Create the kernel task that owns kernel threads */
    task_t *kernel_task = task_create("kernel");
    if (kernel_task) {
        log_printf(LOG_LEVEL_DEBUG, "kernel_task: id=%u\n", kernel_task->task_id);
    }

    /* Create the boot-time kernel pager thread */
    boot_pager_init();

    /* Boot-time self-tests (slow — heap_exhaust drains the heap) */
    // test_run_boot_tests();

    if (bsd_init) {
        log_print(LOG_LEVEL_DEBUG, "arc: calling bsd_init\n");
        bsd_init(boot->cmdline);
        log_print(LOG_LEVEL_INFO, "arc: BSD layer initialized\n");
    } else {
        log_print(LOG_LEVEL_WARN, "arc: no BSD layer — standalone microkernel\n");
    }

    /* Phase 10: bring up all APs (QEMU: make ARCH=amd64 SMP=4 or -smp 4) */
    if (cpu_count() > 1) {
        int online = cpu_start_all();
        log_printf(LOG_LEVEL_INFO, "smp: %u/%u APs online\r\n", online, cpu_count() - 1);
    }

    /* Phase 11: IPI_RESCHEDULE to every AP — checks LAPIC IPI delivery */
    if (cpu_count() > 1) {
        for (unsigned i = 1; i < cpu_count(); i++)
            cpu_send_ipi(cpu_get(i), IPI_RESCHEDULE);

        uint64_t spins = 0;
        int received = 0;
        while (spins++ < 100000000ULL) {
            arch_cpu_relax();
            received = 0;
            for (unsigned i = 1; i < cpu_count(); i++)
                if (cpu_get(i)->arch.need_resched)
                    received++;
            if (received == (int)(cpu_count() - 1))
                break;
        }
        log_printf(LOG_LEVEL_INFO, "smp: IPI_RESCHEDULE delivered to %d/%u CPUs\r\n",
                   received, cpu_count() - 1);
    }

    log_print(LOG_LEVEL_DEBUG, "arc: interrupts enabled\n");
    __asm__ __volatile__("sti");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
