#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ((uint8_t *)dest)[i] = ((const uint8_t *)src)[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ((uint8_t *)s)[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    if ((uintptr_t)dest <= (uintptr_t)src) {
        /* copy forward */
        for (size_t i = 0; i < n; i++) {
            ((uint8_t *)dest)[i] = ((const uint8_t *)src)[i];
        }
    } else {
        /* copy backward */
        for (size_t i = n; i > 0; i--) {
            ((uint8_t *)dest)[i - 1] = ((const uint8_t *)src)[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (((const uint8_t *)s1)[i] != ((const uint8_t *)s2)[i]) {
            return ((const uint8_t *)s1)[i] - ((const uint8_t *)s2)[i];
        }
    }
    return 0;
}

int atoi(const char *s) {
    int n = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n * sign;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
        if (s1[i] == '\0') break;
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++)) ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++)) ;
    return dest;
}
