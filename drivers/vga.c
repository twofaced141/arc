#include <stddef.h>
#include <stdint.h>
#include "idt.h"
#include "terminal.h"
#include "framebuffer.h"

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

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
static uint16_t *terminal_buffer;

extern int fb_active;
static size_t fb_cols, fb_rows;

#define VGA_WORDS_PER_LINE  (VGA_WIDTH)
#define VGA_DWORDS_PER_LINE (VGA_WORDS_PER_LINE / 2)

static void terminal_scroll(void) {
    if (fb_active) {
        fb_scroll();
        terminal_row = fb_rows - 1;
        return;
    }
    uint32_t *buf = (uint32_t *)terminal_buffer;
    uint32_t n = (VGA_HEIGHT - 1) * VGA_DWORDS_PER_LINE;
    for (uint32_t i = 0; i < n; i++)
        buf[i] = buf[i + VGA_DWORDS_PER_LINE];

    uint16_t blank16 = vga_entry(' ', terminal_color);
    uint32_t blank32 = blank16 | ((uint32_t)blank16 << 16);
    uint32_t *last = buf + (VGA_HEIGHT - 1) * VGA_DWORDS_PER_LINE;
    for (uint32_t i = 0; i < VGA_DWORDS_PER_LINE; i++)
        last[i] = blank32;

    terminal_row = VGA_HEIGHT - 1;
}

static void terminal_update_cursor(void) {
    if (fb_active)
        return;
    uint16_t pos = terminal_row * VGA_WIDTH + terminal_column;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void terminal_init(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_buffer = (uint16_t *)VGA_BUFFER;
    uint32_t total = VGA_DWORDS_PER_LINE * VGA_HEIGHT;
    uint16_t blank16 = vga_entry(' ', terminal_color);
    uint32_t blank32 = blank16 | ((uint32_t)blank16 << 16);
    uint32_t *buf = (uint32_t *)terminal_buffer;
    for (uint32_t i = 0; i < total; i++)
        buf[i] = blank32;
    terminal_update_cursor();
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

static void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_init_fb(fb_info_t *info) {
    if (fb_init(info) == 0) {
        fb_active = 1;
        fb_get_dims(&fb_cols, &fb_rows);
        terminal_row = 0;
        terminal_column = 0;
    }
}

static void terminal_sync_cursor(void) {
    if (!fb_active)
        terminal_update_cursor();
}

void terminal_putchar(char c) {
    if (fb_active) {
        if (c == '\n') {
            terminal_column = 0;
            if (++terminal_row == fb_rows) {
                fb_scroll();
                terminal_row = fb_rows - 1;
            }
            return;
        }
        if (c == '\b') {
            if (terminal_column > 0)
                terminal_column--;
            return;
        }
        if (c == '\r') {
            terminal_column = 0;
            return;
        }
        fb_putchar(c, terminal_color, terminal_column, terminal_row);
        if (++terminal_column == fb_cols) {
            terminal_column = 0;
            if (++terminal_row == fb_rows) {
                fb_scroll();
                terminal_row = fb_rows - 1;
            }
        }
        return;
    }
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
        return;
    }
    if (c == '\b') {
        if (terminal_column > 0)
            terminal_column--;
        return;
    }
    if (c == '\r') {
        terminal_column = 0;
        return;
    }
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
    }
}

void terminal_flush(void) {
    terminal_sync_cursor();
}

void terminal_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_print(const char *data) {
    while (*data)
        terminal_putchar(*data++);
}
