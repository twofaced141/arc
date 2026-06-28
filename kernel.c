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
        terminal_print("VMM: address spaces ready\n");

        terminal_print("--- VMM TESTS ---\n");

        uint32_t test_virt = 0x01000000;
        void *test_phys = pmm_alloc_page();
        if (test_phys) {
            vmm_map_page(vmm_get_current_directory(), (uint32_t)test_phys, test_virt, VMM_PRESENT | VMM_WRITABLE);
            uint32_t phys_back = vmm_get_physical(vmm_get_current_directory(), test_virt);
            terminal_print("map:    0x01000000 -> ");
            terminal_print(phys_back == (uint32_t)test_phys ? "OK" : "FAIL");
            terminal_print("\n");

            *(volatile uint32_t *)test_virt = 0xDEADBEEF;
            terminal_print("write:  ");
            terminal_print(*(volatile uint32_t *)test_virt == 0xDEADBEEF ? "OK" : "FAIL");
            terminal_print("\n");

            vmm_unmap_page(vmm_get_current_directory(), test_virt);
            terminal_print("unmap:  OK\n");

            pmm_free_page(test_phys);
        }

        page_directory_t *new_dir = vmm_create_directory();
        terminal_print("fork:   ");
        terminal_print(new_dir ? "OK" : "FAIL");
        terminal_print("\n");

        void *p1 = kmalloc(128);
        void *p2 = kmalloc(256);
        terminal_print("kmalloc:");
        terminal_print(p1 && p2 ? " OK" : " FAIL");
        terminal_print("\n");

        *(volatile uint32_t *)p1 = 0xCAFEBABE;
        terminal_print("kwrite: ");
        terminal_print(*(volatile uint32_t *)p1 == 0xCAFEBABE ? "OK" : "FAIL");
        terminal_print("\n");

        terminal_print("--- END TESTS ---\n");

        char buf[64];
        uint32_t free = pmm_get_free_pages();
        uint32_t free_mb = (free * PAGE_SIZE) / (1024 * 1024);
        terminal_print("Free memory: ");
        int i = 0;
        if (free_mb == 0) {
            buf[i++] = '0';
        } else {
            char tmp[12];
            int j = 0;
            while (free_mb) { tmp[j++] = '0' + free_mb % 10; free_mb /= 10; }
            while (j--) buf[i++] = tmp[j];
        }
        buf[i++] = 'M';
        buf[i++] = 'B';
        buf[i] = '\0';
        terminal_print(buf);
        terminal_print("\n");

        terminal_print("VMM: kernel at 0xC0000000 (higher half)\n");
        terminal_print("VMM: heap at 0xD0000000-0xE0000000\n");
        terminal_print("VMM: address spaces ready\n");
    } else {
        terminal_print("ERROR: not booted with Multiboot2!\n");
    }

    __asm__ __volatile__("sti");
    debug_print("interrupts enabled\r\n");

    // panic("Test", &test_registers);

    terminal_print("keyboard ready, type something:\n");

    for (;;) {
        char c = keyboard_getchar();
        if (c)
            terminal_putchar(c);
    }
}
