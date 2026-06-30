#include <stddef.h>
#include <stdint.h>
#include "terminal.h"
#include "idt.h"
#include "isr.h"
#include "keyboard.h"
#include "debug.h"
#include "panic.h"
#include "gdt.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "scheduler.h"
#include "multiboot2.h"
#include "pci.h"
#include "fs.h"
#include "fd.h"
#include "ata.h"
#include "ext2.h"
#include "rtc.h"
#include "signal.h"
#include "syscalls.h"

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr128(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void idt_install(void) {
    uint16_t code_selector = KERNEL_CS;

    idt_init();

    idt_set_gate(0,  (uint32_t)isr0,  code_selector, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  code_selector, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  code_selector, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  code_selector, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  code_selector, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  code_selector, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  code_selector, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  code_selector, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  code_selector, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  code_selector, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, code_selector, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, code_selector, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, code_selector, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, code_selector, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, code_selector, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, code_selector, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, code_selector, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, code_selector, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, code_selector, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, code_selector, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, code_selector, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, code_selector, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, code_selector, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, code_selector, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, code_selector, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, code_selector, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, code_selector, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, code_selector, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, code_selector, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, code_selector, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, code_selector, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, code_selector, 0x8E);
    idt_set_gate(128, (uint32_t)isr128, code_selector, 0xEE);

    idt_set_gate(32, (uint32_t)irq0,  code_selector, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  code_selector, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  code_selector, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  code_selector, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  code_selector, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  code_selector, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  code_selector, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  code_selector, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  code_selector, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  code_selector, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, code_selector, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, code_selector, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, code_selector, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, code_selector, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, code_selector, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, code_selector, 0x8E);

    pic_remap();
}

void kernel_main(uint32_t mboot_magic, multiboot2_info_t *mboot_info) {
    debug_init();
    terminal_init();
    terminal_print("opencodeOS\n");

    isr_init();
    idt_install();
    keyboard_init();
    pit_init();
    register_interrupt_handler(128, syscall_handler);

    if (mboot_magic == MULTIBOOT2_MAGIC) {
        pmm_init(mboot_info);
        vmm_init();
        vmm_init_heap();
        fs_init(mboot_info);

        pmm_refcount_init();

        process_init();
        scheduler_init();

        rtc_init();
        pci_scan();
        ata_init();


        ext2_fs_t ext2;
        if (ext2_init(&ext2) < 0)
            panic_simple("ext2 init failed");
        fs_set_ext2(&ext2);
        debug_printf("kernel: ext2 mounted\r\n");

        file_t *init_elf = fs_open("/bin/init", EXT2_ROOT_INO);
        if (!init_elf)
            panic_simple("/bin/init not found");

        process_t *proc = process_create_elf(init_elf);
        if (!proc)
            panic_simple("process_create_elf failed");
        scheduler_add_process(proc);
    } else {
        debug_printf("kernel: not booted with Multiboot2\r\n");
    }

    __asm__ __volatile__("sti");

    for (;;)
        __asm__ __volatile__("hlt");
}
