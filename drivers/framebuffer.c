#include "framebuffer.h"
#include "font_data.h"
#include "vmm.h"
#include "debug.h"

extern void *memcpy(void *dst, const void *src, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);

#define FB_VADDR        0xFF000000
#define FONT_WIDTH      8
#define FONT_HEIGHT     16
#define VGA_FG_MASK     0x0F
#define VGA_BG_SHIFT    4

static fb_info_t fb;
int fb_active = 0;
static size_t fb_cols, fb_rows;

static uint32_t vga_color_to_rgb(uint8_t vga_color) {
    static const uint32_t palette[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    return palette[vga_color & 0x0F];
}

static void fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height)
        return;
    uint32_t bpp_bytes = fb.bpp / 8;
    volatile uint8_t *pixel = (volatile uint8_t *)(FB_VADDR + y * fb.pitch + x * bpp_bytes);
    if (bpp_bytes == 4) {
        *(volatile uint32_t *)pixel = color;
    } else if (bpp_bytes == 3) {
        pixel[0] = color & 0xFF;
        pixel[1] = (color >> 8) & 0xFF;
        pixel[2] = (color >> 16) & 0xFF;
    } else if (bpp_bytes == 2) {
        *(volatile uint16_t *)pixel = (uint16_t)(color & 0xFFFF);
    } else if (bpp_bytes == 1) {
        pixel[0] = color & 0xFF;
    }
}

void fb_clear(void) {
    uint32_t bg = vga_color_to_rgb(0);
    for (uint32_t y = 0; y < fb.height; y++)
        for (uint32_t x = 0; x < fb.width; x++)
            fb_putpixel(x, y, bg);
}

void fb_scroll(void) {
    uint32_t char_row_bytes = fb.pitch * FONT_HEIGHT;
    uint32_t total_bytes = fb.pitch * fb.height;
    uint32_t keep_bytes = total_bytes - char_row_bytes;

    memcpy((void *)FB_VADDR, (void *)(FB_VADDR + char_row_bytes), keep_bytes);
    memset((void *)(FB_VADDR + keep_bytes), 0, char_row_bytes);
}

void fb_putchar(char c, uint8_t color, size_t cx, size_t cy) {
    if (!fb_active) return;
    if (cx >= fb_cols || cy >= fb_rows) return;

    uint32_t fg = vga_color_to_rgb(color & 0x0F);
    uint32_t bg = vga_color_to_rgb((color >> 4) & 0x0F);
    uint32_t base_x = cx * FONT_WIDTH;
    uint32_t base_y = cy * FONT_HEIGHT;
    unsigned int ch = (unsigned char)c;
    uint32_t bpp_bytes = fb.bpp / 8;

    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = (ch < 256) ? font_8x16[ch][row] : 0;
        uint32_t *scanline = (uint32_t *)(FB_VADDR + (base_y + row) * fb.pitch + base_x * bpp_bytes);
        scanline[0] = (bits & 0x80) ? fg : bg;
        scanline[1] = (bits & 0x40) ? fg : bg;
        scanline[2] = (bits & 0x20) ? fg : bg;
        scanline[3] = (bits & 0x10) ? fg : bg;
        scanline[4] = (bits & 0x08) ? fg : bg;
        scanline[5] = (bits & 0x04) ? fg : bg;
        scanline[6] = (bits & 0x02) ? fg : bg;
        scanline[7] = (bits & 0x01) ? fg : bg;
    }
}

void fb_get_info(fb_info_t *info) {
    if (info) *info = fb;
}

void fb_set_active(int active) {
    fb_active = active ? 1 : 0;
}

uint32_t fb_get_phys(void) {
    return fb.addr;
}

void fb_get_dims(size_t *cols, size_t *rows) {
    if (cols) *cols = fb_cols;
    if (rows) *rows = fb_rows;
}

int fb_init(fb_info_t *info) {
    fb = *info;
    fb_active = 0;

    if (fb.bpp < 8 || fb.bpp % 8 != 0) {
        debug_printf("fb: unsupported bpp %u\r\n", fb.bpp);
        return -1;
    }

    fb_cols = fb.width / FONT_WIDTH;
    fb_rows = fb.height / FONT_HEIGHT;

    uint32_t fb_size = fb.pitch * fb.height;
    uint32_t pages = (fb_size + 0xFFF) / 0x1000;

    debug_printf("fb: mapping %u pages from phys 0x%x to 0x%x\r\n",
                 pages, fb.addr, FB_VADDR);

    page_directory_t *kdir = vmm_get_kernel_directory();
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = fb.addr + i * 0x1000;
        uint32_t virt = FB_VADDR + i * 0x1000;
        if (vmm_map_page(kdir, phys, virt, VMM_PRESENT | VMM_WRITABLE) < 0) {
            debug_printf("fb: failed to map page %u\r\n", i);
            return -1;
        }
    }

    fb_clear();
    fb_active = 1;
    debug_printf("fb: initialized %ux%u %ubpp, pitch=%u, %ux%u chars\r\n",
                 fb.width, fb.height, fb.bpp, fb.pitch, (unsigned)fb_cols, (unsigned)fb_rows);
    return 0;
}
