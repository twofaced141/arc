#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

char **environ = NULL;

static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count = 0;

typedef struct heap_hdr {
    size_t size;
    struct heap_hdr *next;
    int free;
} heap_hdr_t;

static heap_hdr_t *heap_base;
static heap_hdr_t *heap_end;

static void heap_init(void) {
    void *p = sbrk(0);
    if ((intptr_t)p == -1) return;
    heap_base = (heap_hdr_t *)(((uintptr_t)p + 15) & ~15);
    if (brk(heap_base + 1) < 0) { heap_base = NULL; return; }
    heap_base->size = sizeof(heap_hdr_t);
    heap_base->next = NULL;
    heap_base->free = 0;
    heap_end = heap_base;
}

static heap_hdr_t *find_free(size_t size) {
    heap_hdr_t *h = heap_base;
    while (h) {
        if (h->free && h->size >= size) return h;
        h = h->next;
    }
    return NULL;
}

static void coalesce(heap_hdr_t *h) {
    if (h->next && h->next->free) {
        h->size += h->next->size;
        h->next = h->next->next;
    }
}

void *malloc(size_t size) {
    if (!size) return NULL;
    if (!heap_base) heap_init();
    if (!heap_base) return NULL;

    size = (size + sizeof(heap_hdr_t) + 15) & ~15;
    if (size < sizeof(heap_hdr_t) + 16) size = sizeof(heap_hdr_t) + 16;

    heap_hdr_t *h = find_free(size);
    if (h) {
        size_t rem = h->size - size;
        if (rem >= sizeof(heap_hdr_t) + 16) {
            heap_hdr_t *nh = (heap_hdr_t *)((char *)h + size);
            nh->size = rem;
            nh->next = h->next;
            nh->free = 1;
            h->next = nh;
            h->size = size;
        }
        h->free = 0;
        return (char *)h + sizeof(heap_hdr_t);
    }

    void *old = sbrk((intptr_t)size);
    if ((intptr_t)old == -1) return NULL;

    h = (heap_hdr_t *)old;
    h->size = size;
    h->next = NULL;
    h->free = 0;
    if (heap_end) {
        heap_end->next = h;
        if (heap_end->free) {
            heap_end->size += h->size;
            heap_end->next = h->next;
            h = heap_end;
            h->free = 0;
        }
    }
    heap_end = h;
    return (char *)h + sizeof(heap_hdr_t);
}

void free(void *ptr) {
    if (!ptr) return;
    heap_hdr_t *h = (heap_hdr_t *)((char *)ptr - sizeof(heap_hdr_t));
    h->free = 1;
    coalesce(h);
    heap_hdr_t *c = heap_base;
    while (c && c->next != h) c = c->next;
    if (c && c->free) coalesce(c);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }

    heap_hdr_t *h = (heap_hdr_t *)((char *)ptr - sizeof(heap_hdr_t));
    size_t old = h->size - sizeof(heap_hdr_t);

    void *np = malloc(size);
    if (!np) return NULL;
    size_t copy = old < size ? old : size;
    memcpy(np, ptr, copy);
    free(ptr);
    return np;
}

int atoi(const char *s) {
    int n = 0, sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n * sign;
}

long atol(const char *s) { return atoi(s); }
long long atoll(const char *s) { return atoi(s); }

void exit(int status) {
    while (atexit_count > 0)
        atexit_handlers[--atexit_count]();
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(3), "b"(status), "c"(0), "d"(0), "S"(0));
    for (;;);
}

void abort(void) { exit(EXIT_FAILURE); }

static unsigned int _rand_seed = 1;

int rand(void) {
    _rand_seed = _rand_seed * 1103515245 + 12345;
    return (_rand_seed >> 16) & 0x7FFF;
}

void srand(unsigned int seed) { _rand_seed = seed; }

int abs(int n) { return n < 0 ? -n : n; }

// ----- strtol / strtoul / strtoll / strtoull -----

#ifndef LONG_MIN
#define LONG_MIN  (-2147483647L - 1L)
#endif
#ifndef LONG_MAX
#define LONG_MAX  2147483647L
#endif
#ifndef LLONG_MAX
#define LLONG_MAX  9223372036854775807LL
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX 18446744073709551615ULL
#endif

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    int c, neg = 0, any = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    unsigned long cutoff = (unsigned long)LONG_MAX;
    if (neg) cutoff = (unsigned long)LONG_MAX + 1UL;
    int cutlim = (int)(cutoff % (unsigned long)base);
    cutoff /= (unsigned long)base;

    for (;;) {
        c = (unsigned char)*s++;
        if (c >= '0' && c <= '9')       c -= '0';
        else if (c >= 'a' && c <= 'z')  c -= 'a' - 10;
        else if (c >= 'A' && c <= 'Z')  c -= 'A' - 10;
        else                            { s--; break; }
        if (c >= base)                  { s--; break; }

        if (any < 0) {
        } else if (acc > cutoff || (acc == cutoff && c > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc = acc * (unsigned long)base + (unsigned long)c;
        }
    }

    if (any < 0) {
        acc = neg ? (unsigned long)LONG_MIN : (unsigned long)LONG_MAX;
        errno = ERANGE;
    } else if (neg) {
        acc = (unsigned long)(-(long)acc);
    } else if (!any) {
        s = nptr;
    }

    if (endptr) *endptr = (char *)s;
    return (long)acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    int c, neg = 0, any = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    unsigned long cutoff = (unsigned long)-1 / (unsigned long)base;
    int cutlim = (int)((unsigned long)-1 % (unsigned long)base);

    for (;;) {
        c = (unsigned char)*s++;
        if (c >= '0' && c <= '9')       c -= '0';
        else if (c >= 'a' && c <= 'z')  c -= 'a' - 10;
        else if (c >= 'A' && c <= 'Z')  c -= 'A' - 10;
        else                            { s--; break; }
        if (c >= base)                  { s--; break; }

        if (any < 0) {
        } else if (acc > cutoff || (acc == cutoff && c > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc = acc * (unsigned long)base + (unsigned long)c;
        }
    }

    if (any < 0) {
        acc = (unsigned long)-1;
        errno = ERANGE;
    } else if (neg) {
        acc = (unsigned long)(-(long)acc);
    } else if (!any) {
        s = nptr;
    }

    if (endptr) *endptr = (char *)s;
    return acc;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long long acc = 0;
    int c, neg = 0, any = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    for (;;) {
        c = (unsigned char)*s++;
        if (c >= '0' && c <= '9')       c -= '0';
        else if (c >= 'a' && c <= 'z')  c -= 'a' - 10;
        else if (c >= 'A' && c <= 'Z')  c -= 'A' - 10;
        else                            { s--; break; }
        if (c >= base)                  { s--; break; }

        unsigned long long old = acc;
        acc = acc * (unsigned long long)base + (unsigned long long)c;
        if (acc < old) {
            any = -1;
        } else if (any >= 0) {
            any = 1;
        }
    }

    if (any < 0) {
        acc = neg ? (unsigned long long)LLONG_MIN : (unsigned long long)LLONG_MAX;
        errno = ERANGE;
    } else if (neg) {
        if (acc > (unsigned long long)LLONG_MAX + 1ULL) {
            acc = (unsigned long long)LLONG_MIN;
            errno = ERANGE;
        } else {
            acc = (unsigned long long)(-(long long)acc);
        }
    } else if (acc > (unsigned long long)LLONG_MAX) {
        acc = (unsigned long long)LLONG_MAX;
        errno = ERANGE;
    } else if (!any) {
        s = nptr;
    }

    if (endptr) *endptr = (char *)s;
    return (long long)acc;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long long acc = 0;
    int c, neg = 0, any = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    for (;;) {
        c = (unsigned char)*s++;
        if (c >= '0' && c <= '9')       c -= '0';
        else if (c >= 'a' && c <= 'z')  c -= 'a' - 10;
        else if (c >= 'A' && c <= 'Z')  c -= 'A' - 10;
        else                            { s--; break; }
        if (c >= base)                  { s--; break; }

        unsigned long long old = acc;
        acc = acc * (unsigned long long)base + (unsigned long long)c;
        if (acc < old) {
            any = -1;
        } else if (any >= 0) {
            any = 1;
        }
    }

    if (any < 0) {
        acc = (unsigned long long)-1;
        errno = ERANGE;
    } else if (neg) {
        acc = -(unsigned long long)acc;
    } else if (!any) {
        s = nptr;
    }

    if (endptr) *endptr = (char *)s;
    return acc;
}

// ----- strtod -----

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    double result = 0.0, frac = 0.0, div = 1.0;
    int sign = 1, expsign = 1, exp = 0, digits = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    while (isdigit((unsigned char)*s)) {
        result = result * 10.0 + (double)(*s - '0');
        s++; digits++;
    }

    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            frac = frac * 10.0 + (double)(*s - '0');
            div *= 10.0;
            s++; digits++;
        }
        result += frac / div;
    }

    if ((*s == 'e' || *s == 'E')) {
        s++;
        if (*s == '-') { expsign = -1; s++; }
        else if (*s == '+') s++;
        while (isdigit((unsigned char)*s)) {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        if (expsign < 0) exp = -exp;
    }

    if (!digits) {
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    if (exp > 0)
        while (exp--) result *= 10.0;
    else
        while (exp++) result /= 10.0;

    if (endptr) *endptr = (char *)s;
    return sign * result;
}

// ----- getenv -----

char *getenv(const char *name) {
    if (!environ) return NULL;
    size_t len = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return *e + len + 1;
    }
    return NULL;
}

// ----- atexit -----

int atexit(void (*func)(void)) {
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_handlers[atexit_count++] = func;
    return 0;
}

// ----- qsort -----

static void swap_elem(char *a, char *b, size_t size) {
    char tmp;
    for (size_t i = 0; i < size; i++) {
        tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

static void qsort_helper(char *base, size_t nmemb, size_t size,
                         int (*compar)(const void *, const void *)) {
    if (nmemb <= 1) return;

    size_t last = 0;
    size_t mid = nmemb / 2;

    swap_elem(base + mid * size, base + (nmemb - 1) * size, size);
    char *pivot = base + (nmemb - 1) * size;

    for (size_t i = 0; i < nmemb - 1; i++) {
        if (compar(base + i * size, pivot) < 0) {
            swap_elem(base + i * size, base + last * size, size);
            last++;
        }
    }

    swap_elem(base + last * size, pivot, size);

    if (last > 0)
        qsort_helper(base, last, size, compar);
    if (last + 1 < nmemb)
        qsort_helper(base + (last + 1) * size, nmemb - last - 1, size, compar);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    qsort_helper((char *)base, nmemb, size, compar);
}

// ----- bsearch -----

void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    size_t low = 0, high = nmemb;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int cmp = compar(key, (const char *)base + mid * size);
        if (cmp < 0)
            high = mid;
        else if (cmp > 0)
            low = mid + 1;
        else
            return (void *)((const char *)base + mid * size);
    }

    return NULL;
}

// ----- setenv / unsetenv / putenv / clearenv -----

static int env_find(const char *name, size_t len) {
    if (!environ) return -1;
    int i;
    for (i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=')
            return i;
    }
    return -1;
}

static int env_count(void) {
    if (!environ) return 0;
    int n = 0;
    while (environ[n]) n++;
    return n;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    int idx = env_find(name, nlen);
    if (idx >= 0 && !overwrite) return 0;

    char *entry = (char *)malloc(nlen + vlen + 2);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + vlen + 1] = '\0';

    if (idx >= 0) {
        free(environ[idx]);
        environ[idx] = entry;
        return 0;
    }

    int cnt = env_count();
    char **new = (char **)malloc((cnt + 2) * sizeof(char *));
    if (!new) { free(entry); errno = ENOMEM; return -1; }
    for (int i = 0; i < cnt; i++) new[i] = environ[i];
    new[cnt] = entry;
    new[cnt + 1] = NULL;
    if (environ) free(environ);
    environ = new;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t nlen = strlen(name);
    int idx = env_find(name, nlen);
    if (idx < 0) return 0;

    free(environ[idx]);
    int cnt = env_count();
    for (int i = idx; i < cnt; i++) environ[i] = environ[i + 1];
    return 0;
}

int putenv(char *string) {
    if (!string || !strchr(string, '=')) { errno = EINVAL; return -1; }
    char *eq = strchr(string, '=');
    size_t nlen = eq - string;
    int idx = env_find(string, nlen);
    if (idx >= 0) {
        environ[idx] = string;
        return 0;
    }
    int cnt = env_count();
    char **new = (char **)malloc((cnt + 2) * sizeof(char *));
    if (!new) { errno = ENOMEM; return -1; }
    for (int i = 0; i < cnt; i++) new[i] = environ[i];
    new[cnt] = string;
    new[cnt + 1] = NULL;
    if (environ) free(environ);
    environ = new;
    return 0;
}

int clearenv(void) {
    if (environ) {
        for (char **e = environ; *e; e++) free(*e);
        free(environ);
        environ = NULL;
    }
    return 0;
}

// ----- mkstemp / mkdtemp -----

static int mkstemp_template(char *template) {
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static int seed = 0;
    if (!seed) { seed = getpid() ^ getticks(); }
    for (int attempt = 0; attempt < 100; attempt++) {
        seed = seed * 1103515245 + 12345;
        unsigned int r = (seed >> 16) & 0xFFFFF;
        for (int i = 0; i < 6; i++) {
            int d = r % 36;
            template[len - 6 + i] = d < 10 ? '0' + d : 'a' + d - 10;
            r /= 36;
        }
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *template) {
    return mkstemp_template(template);
}

char *mkdtemp(char *template) {
    int fd = mkstemp_template(template);
    if (fd < 0) return NULL;
    close(fd);
    unlink(template);
    if (mkdir(template, 0700) < 0) return NULL;
    return template;
}

// ----- realpath -----

char *realpath(const char *path, char *resolved_path) {
    static char buf[PATH_MAX];
    if (!resolved_path) resolved_path = buf;

    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i < PATH_MAX - 1) {
            resolved_path[i] = path[i];
            i++;
        }
        resolved_path[i] = '\0';
    } else {
        if (!getcwd(resolved_path, PATH_MAX)) return NULL;
        size_t cwdlen = strlen(resolved_path);
        if (cwdlen + 1 + strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
        resolved_path[cwdlen] = '/';
        strcpy(resolved_path + cwdlen + 1, path);
    }

    char *src = resolved_path;
    char *dst = resolved_path;
    while (*src) {
        if (src[0] == '/' && src[1] == '.' && (src[2] == '/' || src[2] == '\0')) {
            src += 2;
            continue;
        }
        if (src[0] == '/' && src[1] == '.' && src[2] == '.' && (src[3] == '/' || src[3] == '\0')) {
            src += 3;
            if (dst > resolved_path) {
                dst--;
                while (dst > resolved_path && *dst != '/') dst--;
            }
            continue;
        }
        *dst++ = *src++;
    }
    if (dst == resolved_path) *dst++ = '/';
    *dst = '\0';
    return resolved_path;
}

// ----- labs / llabs -----

long labs(long n) { return n < 0 ? -n : n; }
long long llabs(long long n) { return n < 0 ? -n : n; }

// ----- system -----

int system(const char *command) {
    if (!command) return 1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return status;
}

// ----- random / srandom -----

static unsigned long _random_seed = 1;

long random(void) {
    _random_seed = _random_seed * 1103515245 + 12345;
    return (long)((_random_seed >> 16) & 0x7FFFFFFF);
}

void srandom(unsigned int seed) { _random_seed = seed; }
