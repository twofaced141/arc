#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>
#include <stdint.h>
#include "framebuffer.h"

void terminal_init(void);
void terminal_init_fb(fb_info_t *info);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char *data, size_t size);
void terminal_print(const char *data);
void terminal_flush(void);

#endif
