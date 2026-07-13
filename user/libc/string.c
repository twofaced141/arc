#include <string.h>
#include <wchar.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && *s) { n++; s++; }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    return n < (size_t)-1 ? ((unsigned char)*a - (unsigned char)*b) : 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
    return dst - 1;
}

char *stpncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n-- && (*d++ = *src++)) {}
    return d - 1;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n-- && (*d++ = *src++));
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n) {
            int hc = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
            int nc = (*n >= 'A' && *n <= 'Z') ? *n + 32 : *n;
            if (hc != nc) break;
            h++; n++;
        }
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
    if (!needlelen) return (void *)haystack;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        size_t j;
        for (j = 0; j < needlelen; j++) {
            if (h[i + j] != n[j]) break;
        }
        if (j == needlelen) return (void *)(h + i);
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
        s++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    const char *p = s;
    while (*p) {
        int ok = 0;
        for (const char *a = accept; *a; a++) {
            if (*p == *a) { ok = 1; break; }
        }
        if (!ok) break;
        p++;
    }
    return p - s;
}

size_t strcspn(const char *s, const char *reject) {
    const char *p = s;
    while (*p) {
        for (const char *r = reject; *r; r++) {
            if (*p == *r) return p - s;
        }
        p++;
    }
    return p - s;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t slen = strlen(s);
    if (slen > n) slen = n;
    char *p = (char *)malloc(slen + 1);
    if (p) {
        memcpy(p, s, slen);
        p[slen] = '\0';
    }
    return p;
}

char *strtok(char *str, const char *delim) {
    static char *saveptr = NULL;
    return strtok_r(str, delim, &saveptr);
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!str) str = *saveptr;
    if (!str) return NULL;

    while (*str && strchr(delim, *str)) str++;
    if (!*str) { *saveptr = NULL; return NULL; }

    char *token = str;
    while (*str && !strchr(delim, *str)) str++;
    if (*str) { *str++ = '\0'; }
    *saveptr = str;
    return token;
}

char *strsep(char **stringp, const char *delim) {
    char *rv = *stringp;
    if (!rv) return NULL;
    *stringp = NULL;
    char *p = rv;
    while (*p && !strchr(delim, *p)) p++;
    if (*p) { *p++ = '\0'; *stringp = p; }
    return rv;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (*a ? 1 : *b ? -1 : 0);
}

int ffs(int i) {
    if (!i) return 0;
    int n = 1;
    while (!(i & 1)) { i >>= 1; n++; }
    return n;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    if (n == (size_t)-1) return 0;
    return (*a ? 1 : *b ? -1 : 0);
}

char *strerror(int errnum) {
    switch (errnum) {
        case 0: return "Success";
        case EPERM: return "Operation not permitted";
        case ENOENT: return "No such file or directory";
        case ESRCH: return "No such process";
        case EINTR: return "Interrupted system call";
        case EIO: return "Input/output error";
        case ENXIO: return "No such device or address";
        case E2BIG: return "Argument list too long";
        case ENOEXEC: return "Exec format error";
        case EBADF: return "Bad file descriptor";
        case ECHILD: return "No child processes";
        case EAGAIN: return "Resource temporarily unavailable";
        case ENOMEM: return "Cannot allocate memory";
        case EACCES: return "Permission denied";
        case EFAULT: return "Bad address";
        case EBUSY: return "Device or resource busy";
        case EEXIST: return "File exists";
        case EXDEV: return "Invalid cross-device link";
        case ENODEV: return "No such device";
        case ENOTDIR: return "Not a directory";
        case EISDIR: return "Is a directory";
        case EINVAL: return "Invalid argument";
        case ENFILE: return "Too many open files in system";
        case EMFILE: return "Too many open files";
        case ENOTTY: return "Inappropriate ioctl for device";
        case ETXTBSY: return "Text file busy";
        case EFBIG: return "File too large";
        case ENOSPC: return "No space left on device";
        case ESPIPE: return "Illegal seek";
        case EROFS: return "Read-only file system";
        case EMLINK: return "Too many links";
        case EPIPE: return "Broken pipe";
        case EDOM: return "Numerical argument out of domain";
        case ERANGE: return "Numerical result out of range";
        case ENOSYS: return "Function not implemented";
        case ELOOP: return "Too many levels of symbolic links";
        case ENOMSG: return "No message of desired type";
        case EOVERFLOW: return "Value too large for defined data type";
        case ENOTSOCK: return "Socket operation on non-socket";
        case EDESTADDRREQ: return "Destination address required";
        case EMSGSIZE: return "Message too long";
        case EPROTOTYPE: return "Protocol wrong type for socket";
        case ENOPROTOOPT: return "Protocol not available";
        case EPROTONOSUPPORT: return "Protocol not supported";
        case EOPNOTSUPP: return "Operation not supported";
        case EPFNOSUPPORT: return "Protocol family not supported";
        case EAFNOSUPPORT: return "Address family not supported by protocol";
        case EADDRINUSE: return "Address already in use";
        case EADDRNOTAVAIL: return "Cannot assign requested address";
        case ENETDOWN: return "Network is down";
        case ENETUNREACH: return "Network is unreachable";
        case ECONNRESET: return "Connection reset by peer";
        case ENOBUFS: return "No buffer space available";
        case EISCONN: return "Transport endpoint is already connected";
        case ENOTCONN: return "Transport endpoint is not connected";
        case ETIMEDOUT: return "Connection timed out";
        case ECONNREFUSED: return "Connection refused";
        case EHOSTUNREACH: return "No route to host";
        case EALREADY: return "Operation already in progress";
        case EINPROGRESS: return "Operation now in progress";
        default: return "Unknown error";
    }
}

char *strsignal(int signum) {
    switch (signum) {
        case 1: return "Hangup";
        case 2: return "Interrupt";
        case 3: return "Quit";
        case 4: return "Illegal instruction";
        case 5: return "Trace/breakpoint trap";
        case 6: return "Aborted";
        case 7: return "Bus error";
        case 8: return "Floating point exception";
        case 9: return "Killed";
        case 10: return "User defined signal 1";
        case 11: return "Segmentation fault";
        case 12: return "User defined signal 2";
        case 13: return "Broken pipe";
        case 14: return "Alarm clock";
        case 15: return "Terminated";
        default: return "Unknown signal";
    }
}

void *memcpy(void *dst, const void *src, size_t n) {
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    char *d = dst;
    const char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memccpy(void *dest, const void *src, int c, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s;
        if (*s++ == (unsigned char)c) return d;
    }
    return NULL;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

int wcwidth(wchar_t wc) {
    if (wc == 0) return 0;
    if (wc < 0x20 || (wc >= 0x7F && wc < 0xA0)) return -1;
    if (wc >= 0x1100 &&
        (wc <= 0x115F || wc == 0x2329 || wc == 0x232A ||
         (wc >= 0x2E80 && wc <= 0x303E) ||
         (wc >= 0x3040 && wc <= 0x33FF) ||
         (wc >= 0x3400 && wc <= 0x4DB5) ||
         (wc >= 0x4E00 && wc <= 0xA4CF) ||
         (wc >= 0xA960 && wc <= 0xA97C) ||
         (wc >= 0xAC00 && wc <= 0xD7A3) ||
         (wc >= 0xD800 && wc <= 0xFAFF) ||
         (wc >= 0xFE10 && wc <= 0xFE19) ||
         (wc >= 0xFE30 && wc <= 0xFE6F) ||
         (wc >= 0xFF01 && wc <= 0xFF60) ||
         (wc >= 0xFFE0 && wc <= 0xFFE6) ||
         (wc >= 0x1B000 && wc <= 0x1B0FF) ||
         (wc >= 0x1D300 && wc <= 0x1D35F) ||
         (wc >= 0x20000 && wc <= 0x2FA1F)))
        return 2;
    return 1;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
    (void)ps;
    if (!s) return 1;
    if ((unsigned int)wc < 0x80) {
        s[0] = (char)wc;
        return 1;
    } else if ((unsigned int)wc < 0x800) {
        s[0] = 0xC0 | (wc >> 6);
        s[1] = 0x80 | (wc & 0x3F);
        return 2;
    } else if ((unsigned int)wc < 0x10000) {
        s[0] = 0xE0 | (wc >> 12);
        s[1] = 0x80 | ((wc >> 6) & 0x3F);
        s[2] = 0x80 | (wc & 0x3F);
        return 3;
    } else {
        s[0] = 0xF0 | (wc >> 18);
        s[1] = 0x80 | ((wc >> 12) & 0x3F);
        s[2] = 0x80 | ((wc >> 6) & 0x3F);
        s[3] = 0x80 | (wc & 0x3F);
        return 4;
    }
}
