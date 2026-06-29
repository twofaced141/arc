#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include <stdarg.h>

void debug_init(void);
void debug_putchar(char c);
void debug_print(const char *s);
void debug_println(const char *s);
void debug_print_hex8(uint8_t value);
void debug_print_hex16(uint16_t value);
void debug_print_hex32(uint32_t value);
void debug_print_dec(uint32_t value);
void debug_printf(const char *fmt, ...);

#endif
