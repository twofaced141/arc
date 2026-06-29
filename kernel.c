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

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN   = 14,
    VGA_WHITE         = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | (uint16_t)color << 8;
}

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t *terminal_buffer;

void terminal_init(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_WHITE, VGA_BLACK);
    terminal_buffer = (uint16_t *)VGA_BUFFER;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
        return;
    }
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

void terminal_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_print(const char *data) {
    while (*data)
        terminal_putchar(*data++);
}

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
    uint32_t esp_val;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(esp_val));
    debug_print("kernel_main esp=0x");
    debug_print_hex32(esp_val);
    debug_print("\r\n");

    terminal_init();
    terminal_print("Hello from base kernel!\n");
    terminal_print("Multiboot2 compliant on x86\n");

    debug_init();
    debug_print("serial alive\r\n");

    isr_init();
    debug_print("isr_init done\r\n");

    idt_install();
    debug_print("idt_install done\r\n");

    keyboard_init();
    debug_print("keyboard_init done\r\n");

    pit_init();
    debug_print("pit_init done\r\n");

    if (mboot_magic == MULTIBOOT2_MAGIC) {
        pmm_init(mboot_info);
        debug_print("pmm_init done\r\n");

        vmm_init();
        debug_print("vmm_init done\r\n");

        vmm_init_heap();
        debug_print("vmm_init_heap done\r\n");

        terminal_print("VMM: kernel at 0xC0000000 (higher half)\n");
        terminal_print("VMM: heap at 0xD0000000-0xE0000000\n");
        terminal_print("HEAP: testing allocator...\n");
        void *a1 = kmalloc(32);
        void *a2 = kmalloc(64);
        void *a3 = kmalloc(128);
        debug_printf("heap: a1=0x%x a2=0x%x a3=0x%x\r\n", a1, a2, a3);
        if (a1 && a2 && a3) {
            __builtin_memset(a1, 0xAA, 32);
            __builtin_memset(a2, 0xBB, 64);
            __builtin_memset(a3, 0xCC, 128);
        }
        terminal_print("HEAP: allocated 32+64+128, fill ok\n");

        kfree(a2);
        terminal_print("HEAP: freed middle block\n");
        void *a4 = kmalloc(32);
        debug_printf("heap: a4=0x%x (should reuse a2)\r\n", a4);
        terminal_print("HEAP: alloc 32 after free");
        terminal_print(a4 ? " OK\n" : " FAIL\n");

        kfree(a1);
        kfree(a3);
        terminal_print("HEAP: freed all\n");
        void *a5 = kmalloc(256);
        terminal_print("HEAP: alloc 256 after full free");
        terminal_print(a5 ? " OK\n" : " FAIL\n");
        __builtin_memset(a5, 0xDD, 256);
        kfree(a5);
        terminal_print("HEAP: all tests done\n");
        terminal_print("VMM: address spaces ready\n");

        process_init();
        debug_print("process_init done\r\n");
        scheduler_init();
        debug_print("scheduler_init done\r\n");

        uint8_t user_code[] = {
            0xBB, 0x41, 0x00, 0x00, 0x00,
            0xB8, 0x01, 0x00, 0x00, 0x00,
            0xCD, 0x80,
            0xB8, 0x02, 0x00, 0x00, 0x00,
            0xCD, 0x80,
            0xEB, 0xF5
        };
        process_t *proc = process_create_user(0x00100000, user_code, sizeof(user_code));
        if (proc) {
            scheduler_add_process(proc);
            terminal_print("SCHEDULER: user process created (pid=");
            terminal_putchar('0' + proc->pid);
            terminal_print(")\n");

        } else {
            terminal_print("SCHEDULER: failed to create process\n");
        }
    } else {
        terminal_print("ERROR: not booted with Multiboot2!\n");
    }

    debug_print("before sti\r\n");
    __asm__ __volatile__("sti");
    debug_print("after sti\r\n");

    // panic("Test", &test_registers);

    terminal_print("keyboard ready, type something:\n");

    for (;;) {
        char c = keyboard_getchar();
        if (c)
            terminal_putchar(c);
    }
}
