#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t type;
} fb_info_t;

int  fb_init(fb_info_t *info);
void fb_get_info(fb_info_t *info);
void fb_clear(void);
void fb_scroll(void);
void fb_putchar(char c, uint8_t color, size_t cx, size_t cy);
void fb_get_dims(size_t *cols, size_t *rows);
uint32_t fb_get_phys(void);
void fb_set_active(int active);

#endif
