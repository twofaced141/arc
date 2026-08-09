#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

void terminal_init(void);
void terminal_putchar(char c);
void terminal_write(const char *data, uint32_t size);
void terminal_print(const char *s);
void terminal_print_hex32(uint32_t value);
void terminal_print_dec(uint32_t value);
void terminal_flush(void);

#endif
