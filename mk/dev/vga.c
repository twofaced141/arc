#ifndef __aarch64__
/* vga.c — x86 (amd64/i386) only; VGA text-mode is PC-specific */

#include "terminal.h"
#include "idt.h"

#define VGA_MEMORY 0xB8000
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static uint16_t *vga_buffer;
static int vga_row;
static int vga_col;
static uint8_t vga_color;

void terminal_init(void) {
    vga_buffer = (uint16_t *)VGA_MEMORY;
    vga_row = 0;
    vga_col = 0;
    vga_color = 0x0F; /* White on black */
    
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = (uint16_t)' ' | (uint16_t)vga_color << 8;
        }
    }
}

static void scroll(void) {
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (uint16_t)' ' | (uint16_t)vga_color << 8;
    vga_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) scroll();
        return;
    }
    if (c == '\r') {
        vga_col = 0;
        return;
    }
    if (c == '\t') {
        vga_col = (vga_col + 8) & ~7;
        return;
    }
    
    vga_buffer[vga_row * VGA_WIDTH + vga_col] = (uint16_t)c | (uint16_t)vga_color << 8;
    vga_col++;
    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) scroll();
    }
}

void terminal_write(const char *data, uint32_t size) {
    for (uint32_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_print(const char *s) {
    while (*s) terminal_putchar(*s++);
}

void terminal_print_hex32(uint32_t value) {
    const char hex[] = "0123456789ABCDEF";
    terminal_print("0x");
    terminal_putchar(hex[(value >> 28) & 0xF]);
    terminal_putchar(hex[(value >> 24) & 0xF]);
    terminal_putchar(hex[(value >> 20) & 0xF]);
    terminal_putchar(hex[(value >> 16) & 0xF]);
    terminal_putchar(hex[(value >> 12) & 0xF]);
    terminal_putchar(hex[(value >> 8) & 0xF]);
    terminal_putchar(hex[(value >> 4) & 0xF]);
    terminal_putchar(hex[value & 0xF]);
}

void terminal_print_dec(uint32_t value) {
    if (value == 0) { terminal_putchar('0'); return; }
    char buf[12]; int i = 0;
    while (value > 0) { buf[i++] = '0' + (value % 10); value /= 10; }
    while (i > 0) terminal_putchar(buf[--i]);
}

void terminal_flush(void) {
    /* VGA text mode doesn't need flushing */
}

#endif /* !__aarch64__ */
