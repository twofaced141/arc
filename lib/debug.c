#include "debug.h"
#include "idt.h"

static const char hex_digits[] = "0123456789ABCDEF";

void debug_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

void debug_putchar(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, c);
}

void debug_print(const char *s) {
    while (*s)
        debug_putchar(*s++);
}

void debug_println(const char *s) {
    debug_print(s);
    debug_print("\r\n");
}

void debug_print_hex8(uint8_t value) {
    debug_putchar(hex_digits[(value >> 4) & 0xF]);
    debug_putchar(hex_digits[value & 0xF]);
}

void debug_print_hex16(uint16_t value) {
    debug_print_hex8((uint8_t)(value >> 8));
    debug_print_hex8((uint8_t)(value & 0xFF));
}

void debug_print_hex32(uint32_t value) {
    debug_print_hex16((uint16_t)(value >> 16));
    debug_print_hex16((uint16_t)(value & 0xFFFF));
}

void debug_print_dec(uint32_t value) {
    char buf[12];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (value == 0) {
        debug_putchar('0');
        return;
    }
    while (value > 0) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }
    debug_print(&buf[i]);
}

void debug_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            debug_putchar(*p);
            continue;
        }
        p++;
        switch (*p) {
        case 's': {
            const char *s = va_arg(args, const char *);
            debug_print(s ? s : "(null)");
            break;
        }
        case 'd': {
            int32_t val = va_arg(args, int32_t);
            if (val < 0) {
                debug_putchar('-');
                val = -val;
            }
            debug_print_dec((uint32_t)val);
            break;
        }
        case 'u': {
            uint32_t val = va_arg(args, uint32_t);
            debug_print_dec(val);
            break;
        }
        case 'x': {
            uint32_t val = va_arg(args, uint32_t);
            int started = 0;
            for (int i = 28; i >= 0; i -= 4) {
                uint8_t nibble = (val >> i) & 0xF;
                if (nibble || started || i == 0) {
                    debug_putchar(hex_digits[nibble]);
                    started = 1;
                }
            }
            break;
        }
        case 'p': {
            uint32_t val = va_arg(args, uint32_t);
            debug_print("0x");
            debug_print_hex32(val);
            break;
        }
        case 'c':
            debug_putchar((char)va_arg(args, int));
            break;
        case '%':
            debug_putchar('%');
            break;
        default:
            debug_putchar('%');
            debug_putchar(*p);
            break;
        }
    }

    va_end(args);
}
