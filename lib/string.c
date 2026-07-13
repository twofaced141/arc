#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t i = 0;
    if ((uintptr_t)d % 4 == 0 && (uintptr_t)s % 4 == 0) {
        uint32_t *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        while (i + 4 <= n) { *dw++ = *sw++; i += 4; }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    for (; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    size_t i = 0;
    if ((uintptr_t)p % 4 == 0) {
        uint32_t cc = (unsigned char)c;
        cc |= cc << 8 | cc << 16 | cc << 24;
        uint32_t *pw = (uint32_t *)p;
        while (i + 4 <= n) { *pw++ = cc; i += 4; }
        p = (unsigned char *)pw;
    }
    for (; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

int atoi(const char *s) {
    int n = 0, sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return sign * n;
}
