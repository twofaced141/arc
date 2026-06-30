#include <stdlib.h>
#include <unistd.h>
#include <string.h>

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

void exit(int status) {
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
