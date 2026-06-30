#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

int putchar(int c) {
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(1), "b"((char)c), "c"(0), "d"(0), "S"(0)
        : "memory");
    return (unsigned char)c;
}

int puts(const char *s) {
    while (*s) putchar(*s++);
    putchar('\n');
    return 0;
}

static void emit(char **buf, int *rem, char c) {
    if (*rem > 1) { *(*buf)++ = c; (*rem)--; }
}

static void emit_str(char **buf, int *rem, const char *s) {
    while (*s && *rem > 1) emit(buf, rem, *s++);
}

static void emit_pad(char **buf, int *rem, int count, char c) {
    while (count-- > 0 && *rem > 1) emit(buf, rem, c);
}

static char *utoa(unsigned int val, char *end) {
    *--end = '\0';
    if (val == 0) *--end = '0';
    while (val > 0) {
        *--end = '0' + (val % 10);
        val /= 10;
    }
    return end;
}

static char *utoa_hex(unsigned int val, char *end, int upper) {
    *--end = '\0';
    if (val == 0) *--end = '0';
    while (val > 0) {
        unsigned int d = val % 16;
        *--end = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
        val /= 16;
    }
    return end;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    char *start = buf;
    int rem = (int)size;
    char tmp[32];

    if (size == 0) return 0;

    while (*fmt && rem > 1) {
        if (*fmt != '%') {
            emit(&buf, &rem, *fmt++);
            continue;
        }
        fmt++;

        int pad_zero = 0;
        int left = 0;
        int width = 0;
        int long_mod = 0;

        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') { long_mod = 1; fmt++; }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int n = long_mod ? (int)va_arg(ap, long) : va_arg(ap, int);
                unsigned int u;
                int neg = 0;
                if (n < 0) { neg = 1; u = (unsigned int)(-n); }
                else { u = (unsigned int)n; }
                char *s = utoa(u, tmp + sizeof(tmp));
                int slen = (tmp + sizeof(tmp) - 1) - s;
                if (neg) slen++;
                int pad = width > slen ? width - slen : 0;
                if (neg) emit(&buf, &rem, '-');
                if (!left && pad_zero) emit_pad(&buf, &rem, pad, '0');
                else if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 'u': {
                unsigned int u = long_mod ? (unsigned long)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char *s = utoa(u, tmp + sizeof(tmp));
                int slen = (tmp + sizeof(tmp) - 1) - s;
                int pad = width > slen ? width - slen : 0;
                if (!left && pad_zero) emit_pad(&buf, &rem, pad, '0');
                else if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 'x':
            case 'X': {
                unsigned int x = long_mod ? (unsigned long)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char *s = utoa_hex(x, tmp + sizeof(tmp), *fmt == 'X');
                int slen = (tmp + sizeof(tmp) - 1) - s;
                int pad = width > slen ? width - slen : 0;
                if (!left && pad_zero) emit_pad(&buf, &rem, pad, '0');
                else if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int slen = strlen(s);
                int pad = width > slen ? width - slen : 0;
                if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                emit(&buf, &rem, c);
                fmt++;
                break;
            }
            case '%':
                emit(&buf, &rem, '%');
                fmt++;
                break;
            default:
                emit(&buf, &rem, '%');
                fmt++;
                break;
        }
    }

    *buf = '\0';
    return buf - start;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(1, buf, n);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(1, buf, n);
    return n;
}

int getchar(void) {
    char c;
    while (read(0, &c, 1) <= 0) yield();
    return (unsigned char)c;
}

void perror(const char *s) {
    if (s) write(2, s, strlen(s));
    write(2, "\n", 1);
}
