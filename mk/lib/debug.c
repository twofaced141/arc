#include "debug.h"
#include "spinlock.h"
#include "string.h"
#include <stdarg.h>
#include <stddef.h>

static spinlock_t serial_lock = SPINLOCK_INIT;
static volatile log_level_t g_log_level = LOG_LEVEL_INFO;

#ifndef __aarch64__
#include "serial.h"
#include "terminal.h"
static void debug_putchar_actual(char c) { serial_putchar(c); }
void debug_init(void) { serial_init(); }
#else
extern void uart_putchar(char c);
static void debug_putchar_actual(char c) { uart_putchar(c); }
void debug_init(void) { /* UART init done in arm64 main.c */ }
#endif

static inline void dp_raw(char c) { debug_putchar_actual(c); }

static void print_raw(const char *s) {
    while (*s) dp_raw(*s++);
}

static void print_hex8_raw(uint8_t v) {
    const char hex[] = "0123456789ABCDEF";
    dp_raw(hex[(v >> 4) & 0xF]);
    dp_raw(hex[v & 0xF]);
}

static void print_hex16_raw(uint16_t v) {
    print_hex8_raw((uint8_t)(v >> 8));
    print_hex8_raw((uint8_t)v);
}

static void print_hex32_raw(uint32_t v) {
    print_hex8_raw((uint8_t)(v >> 24));
    print_hex8_raw((uint8_t)(v >> 16));
    print_hex8_raw((uint8_t)(v >> 8));
    print_hex8_raw((uint8_t)v);
}

static void print_hex64_raw(uint64_t v) {
    print_hex32_raw((uint32_t)(v >> 32));
    print_hex32_raw((uint32_t)v);
}

static void print_dec_raw(uint32_t v) {
    if (v == 0) {
        dp_raw('0');
    } else {
        char buf[12];
        int i = 0;
        while (v > 0) {
            buf[i++] = '0' + (v % 10);
            v /= 10;
        }
        while (i > 0) dp_raw(buf[--i]);
    }
}

static void print_hex_digits_raw(uint64_t v, int width, int zero_pad, int upper) {
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int nd = 0;
    uint64_t t = v;
    do { nd++; t >>= 4; } while (t);
    for (int i = nd; i < width; i++) dp_raw(zero_pad ? '0' : ' ');
    for (int i = nd - 1; i >= 0; i--)
        dp_raw(hex[(v >> (i * 4)) & 0xF]);
}

static void print_dec_digits_raw(uint64_t v, int width, int zero_pad) {
    char buf[24];
    int i = 24, nd = 0;
    do { buf[--i] = '0' + (v % 10); v /= 10; nd++; } while (v);
    for (int k = nd; k < width; k++) dp_raw(zero_pad ? '0' : ' ');
    while (i < 24) dp_raw(buf[i++]);
}

static void print_oct_digits_raw(uint64_t v, int width, int zero_pad) {
    char buf[24];
    int i = 24, nd = 0;
    do { buf[--i] = '0' + (v % 8); v /= 8; nd++; } while (v);
    for (int k = nd; k < width; k++) dp_raw(zero_pad ? '0' : ' ');
    while (i < 24) dp_raw(buf[i++]);
}

void debug_putchar(char c) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    dp_raw(c);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_write(const char *buf, unsigned int count) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    for (unsigned int i = 0; i < count; i++) dp_raw(buf[i]);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_print(const char *s) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_raw(s);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_println(const char *s) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_raw(s);
    dp_raw('\r');
    dp_raw('\n');
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_print_hex8(uint8_t v) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_hex8_raw(v);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_print_hex16(uint16_t v) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_hex16_raw(v);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_print_hex32(uint32_t v) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_hex32_raw(v);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_print_hex64(uint64_t v) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_hex64_raw(v);
    spin_unlock_irqrestore(&serial_lock, flags);
}

void debug_print_dec(uint32_t v) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    print_dec_raw(v);
    spin_unlock_irqrestore(&serial_lock, flags);
}

static void vprintf_locked(const char *fmt, va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            dp_raw(*p);
            continue;
        }
        p++;
        int width = 0;
        int zero_pad = 0;
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        int is_long = 0, is_longlong = 0, is_size = 0;
        if (*p == 'l') { is_long = 1; p++; if (*p == 'l') { is_longlong = 1; p++; } }
        else if (*p == 'z') { is_size = 1; p++; }
        switch (*p) {
            case 'd': {
                int64_t v;
                if (is_longlong) v = (int64_t)va_arg(args, long long);
                else if (is_long) v = (int64_t)va_arg(args, long);
                else if (is_size) v = (int64_t)va_arg(args, long);
                else v = va_arg(args, int);
                if (v < 0) { dp_raw('-'); v = -v; }
                print_dec_digits_raw((uint64_t)v, width, zero_pad);
                break;
            }
            case 'u': {
                uint64_t v;
                if (is_longlong) v = (uint64_t)va_arg(args, unsigned long long);
                else if (is_long) v = (uint64_t)va_arg(args, unsigned long);
                else if (is_size) v = (uint64_t)va_arg(args, size_t);
                else v = va_arg(args, unsigned int);
                print_dec_digits_raw(v, width, zero_pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v;
                if (is_longlong) v = (uint64_t)va_arg(args, unsigned long long);
                else if (is_long) v = (uint64_t)va_arg(args, unsigned long);
                else if (is_size) v = (uint64_t)va_arg(args, size_t);
                else v = va_arg(args, unsigned int);
                print_hex_digits_raw(v, width, zero_pad, *p == 'X');
                break;
            }
            case 'o': {
                uint64_t v;
                if (is_longlong) v = (uint64_t)va_arg(args, unsigned long long);
                else if (is_long) v = (uint64_t)va_arg(args, unsigned long);
                else if (is_size) v = (uint64_t)va_arg(args, size_t);
                else v = va_arg(args, unsigned int);
                print_oct_digits_raw(v, width, zero_pad);
                break;
            }
            case 'p':
                print_hex_digits_raw(va_arg(args, uintptr_t), 0, 0, 0);
                break;
            case 's':
                print_raw(va_arg(args, const char *));
                break;
            case 'c':
                dp_raw((char)va_arg(args, int));
                break;
            case '%':
                dp_raw('%');
                break;
            default:
                dp_raw('%');
                dp_raw(*p);
                break;
        }
    }
}

void debug_printf(const char *fmt, ...) {
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);

    va_list args;
    va_start(args, fmt);
    vprintf_locked(fmt, args);
    va_end(args);

    spin_unlock_irqrestore(&serial_lock, flags);
}

/* Lock-free raw output: the caller (panic) has exclusive UART ownership. */
void debug_print_raw(const char *s) {
    print_raw(s);
}

void debug_printf_raw(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf_locked(fmt, args);
    va_end(args);
}

/* ── Leveled logging ─────────────────────────────────────────────── */

void log_set_level(log_level_t level) { g_log_level = level; }

log_level_t log_get_level(void) { return g_log_level; }

void log_print(log_level_t level, const char *s) {
    if (level > g_log_level)
        return;
    debug_print(s);
}

void log_printf(log_level_t level, const char *fmt, ...) {
    if (level > g_log_level)
        return;
    uint32_t flags;
    spin_lock_irqsave(&serial_lock, &flags);

    va_list args;
    va_start(args, fmt);
    vprintf_locked(fmt, args);
    va_end(args);

    spin_unlock_irqrestore(&serial_lock, flags);
}

void log_parse_cmdline(const char *cmdline) {
    if (!cmdline)
        return;
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',')
            p++;
        if (p - tok >= 9 && memcmp(tok, "loglevel=", 9) == 0) {
            const char *v = tok + 9;
            if (!*v)
                break;
            if (v[0] >= '0' && v[0] <= '3' && !v[1])
                g_log_level = (log_level_t)(v[0] - '0');
            else if (strcmp(v, "error") == 0)
                g_log_level = LOG_LEVEL_ERROR;
            else if (strcmp(v, "warn") == 0 || strcmp(v, "warning") == 0)
                g_log_level = LOG_LEVEL_WARN;
            else if (strcmp(v, "info") == 0)
                g_log_level = LOG_LEVEL_INFO;
            else if (strcmp(v, "debug") == 0)
                g_log_level = LOG_LEVEL_DEBUG;
            break;
        }
    }
}
