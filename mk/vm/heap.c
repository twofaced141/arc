/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Unified heap HAL — single implementation for all architectures.
 * x86 (i386/amd64): demand-mapped heap at HEAP_START..HEAP_END
 * arm64: static 8M heap over __heap_start..__heap_end (linker-provided)
 */

#include "memory.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include "string.h"
#include "debug.h"
#include <stdint.h>

#if defined(__aarch64__)

/* ---------- arm64: static heap over linker region ---------- */
typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    struct heap_block *next;
} heap_block_t;

#define HEAP_MAGIC_FREE 0x48454150
#define HEAP_MAGIC_USED 0x44454144
#define HEAP_SIZE_MASK  0x7FFFFFFF

extern char __heap_start[];
extern char __heap_end[];

static heap_block_t *heap_base;
static heap_block_t *heap_free_list;
static int heap_initialized;
static spinlock_t heap_lock = SPINLOCK_INIT;

static void heap_do_init(void) {
    heap_base = (heap_block_t *)__heap_start;
    memset(heap_base, 0, sizeof(heap_block_t));
    heap_base->magic = HEAP_MAGIC_FREE;
    heap_base->size = (uint64_t)(__heap_end - __heap_start) - sizeof(heap_block_t);
    heap_base->next = NULL;
    heap_free_list = heap_base;
    heap_initialized = 1;
}

void heap_init(void) {
    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);
    if (!heap_initialized) heap_do_init();
    spin_unlock_irqrestore(&heap_lock, flags);
}

void vmm_init_heap(void) {
    heap_init();
    log_print(LOG_LEVEL_INFO, "heap: arm64 static heap ready\r\n");
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);
    if (!heap_initialized) heap_do_init();
    size = (size + 3) & ~3;
    if (size < 16) size = 16;
    heap_block_t *prev = NULL;
    heap_block_t *block = heap_free_list;
    while (block) {
        if (block->magic != HEAP_MAGIC_FREE) {
            heap_free_list = block->next;
            block = heap_free_list;
            prev = NULL;
            continue;
        }
        uint32_t block_size = block->size & HEAP_SIZE_MASK;
        if (block_size >= size) {
            if (block_size >= size + sizeof(heap_block_t) + 16) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + sizeof(heap_block_t) + size);
                new_block->magic = HEAP_MAGIC_FREE;
                new_block->size = block_size - size - sizeof(heap_block_t);
                new_block->next = block->next;
                block->size = size | HEAP_MAGIC_USED;
                block->next = new_block;
            } else {
                block->magic = HEAP_MAGIC_USED;
            }
            if (prev)
                prev->next = block->next;
            else
                heap_free_list = block->next;
            spin_unlock_irqrestore(&heap_lock, flags);
            memset((uint8_t *)block + sizeof(heap_block_t), 0, size);
            return (void *)((uint8_t *)block + sizeof(heap_block_t));
        }
        prev = block;
        block = block->next;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return NULL;
}

void *kcalloc(uint32_t count, uint32_t size) {
    if (count != 0 && size > 0xFFFFFFFFU / count)
        return NULL;
    uint32_t total = count * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void kfree(void *addr) {
    if (!addr) return;
    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);
    heap_block_t *block = (heap_block_t *)((uint8_t *)addr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC_USED) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }
    block->magic = HEAP_MAGIC_FREE;
    block->next = heap_free_list;
    heap_free_list = block;
    spin_unlock_irqrestore(&heap_lock, flags);
}

#else /* x86 (i386 / amd64): dynamic demand-mapped heap */

/* Generic heap using HEAP_START/END from memory.h */
typedef struct heap_block {
    uintptr_t size; /* low bit = HEAP_BLOCK_FREE */
    union {
        struct heap_block *next;
        uintptr_t magic;
    };
} heap_block_t;

#if defined(__x86_64__)
#define HEAP_MAGIC_USED    0x4845415055534544ULL
#else
#define HEAP_MAGIC_USED    0x48454150ULL
#endif
#define HEAP_BLOCK_FREE    1
#define HEAP_SIZE_MASK    (~(uintptr_t)1)
#define HEAP_HEADER_SIZE   sizeof(heap_block_t)
#define HEAP_ALIGNMENT     16
#define HEAP_ALIGN(sz)     (((sz) + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1))
#define HEAP_MIN_BLOCK     (HEAP_ALIGN(HEAP_HEADER_SIZE + HEAP_ALIGNMENT))
/* i386 header is 8 bytes, so block starts are only 8-aligned
 * (unlike amd64 where both are 16). Address/size validation must use
 * BLOCK_ALIGN, size rounding stays 16. */
#if defined(__i386__) || defined(__i686__)
#define HEAP_BLOCK_ALIGN   8
#else
#define HEAP_BLOCK_ALIGN   HEAP_ALIGNMENT
#endif
#define HEAP_MAX_WALK      (8 * 1024 * 1024)

static heap_block_t *heap_free_list;
static uintptr_t heap_mapped_end;
static uintptr_t heap_brk;
static spinlock_t heap_lock = SPINLOCK_INIT;

static int heap_map_until(uintptr_t addr) {
    page_directory_t *kdir = vmm_get_kernel_directory();
    while (heap_mapped_end < addr) {
        void *phys = pmm_alloc_page();
        if (!phys) return -1;
        if (vmm_map_page(kdir, (uint64_t)(uintptr_t)phys, (uint64_t)heap_mapped_end,
                         VMM_PRESENT | VMM_WRITABLE) < 0) {
            pmm_free_page(phys);
            return -1;
        }
        heap_mapped_end += PAGE_SIZE;
    }
    return 0;
}

static int heap_size_sane(uintptr_t sz, uintptr_t baddr) {
    uintptr_t body = sz & HEAP_SIZE_MASK;
    if (body < HEAP_MIN_BLOCK) return 0;
    if (body & (HEAP_BLOCK_ALIGN - 1)) return 0;
    if (body > heap_brk - baddr) return 0;
    return 1;
}

static void heap_coalesce(heap_block_t *b) {
    uintptr_t body = b->size & HEAP_SIZE_MASK;
    heap_block_t *next = (heap_block_t *)((uint8_t *)b + body);
    if (next != b->next) return;
    if ((uintptr_t)next + HEAP_HEADER_SIZE > heap_brk) return;
    if ((uintptr_t)next & (HEAP_BLOCK_ALIGN - 1)) return;
    uintptr_t nsize = next->size;
    if (!(nsize & HEAP_BLOCK_FREE)) return;
    if (!heap_size_sane(nsize, (uintptr_t)next)) return;
    b->size = (body + (nsize & HEAP_SIZE_MASK)) | HEAP_BLOCK_FREE;
    b->next = next->next;
}

static void heap_shrink(void) {
    for (;;) {
        heap_block_t *prev = NULL;
        heap_block_t *cur = heap_free_list;
        heap_block_t *tail = NULL;
        heap_block_t *tail_prev = NULL;
        for (uintptr_t steps = 0; cur && steps < HEAP_MAX_WALK; steps++) {
            uintptr_t caddr = (uintptr_t)cur;
            if (caddr < HEAP_START || caddr + HEAP_HEADER_SIZE > heap_brk ||
                (caddr & (HEAP_BLOCK_ALIGN - 1)))
                break;
            uintptr_t csize = cur->size & HEAP_SIZE_MASK;
            if (!(cur->size & HEAP_BLOCK_FREE)) break;
            if (csize < HEAP_MIN_BLOCK || (csize & (HEAP_BLOCK_ALIGN - 1)) ||
                csize > heap_brk - caddr)
                break;
            if ((uint8_t *)cur + csize == (uint8_t *)heap_brk) {
                tail = cur;
                tail_prev = prev;
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        if (!tail) break;
        heap_brk = (uintptr_t)tail;
        if (tail_prev)
            tail_prev->next = tail->next;
        else
            heap_free_list = tail->next;
    }
    uintptr_t keep = (heap_brk + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t floor = HEAP_START + HEAP_INITIAL_PAGES * PAGE_SIZE;
    if (keep < floor) keep = floor;
    while (heap_mapped_end > keep) {
        uintptr_t addr = heap_mapped_end - PAGE_SIZE;
#if defined(__i386__) || defined(__i686__)
        uint32_t phys = vmm_get_physical(vmm_get_kernel_directory(), (uint32_t)addr);
        vmm_unmap_page(vmm_get_kernel_directory(), (uint32_t)addr);
#else
        uint64_t phys = vmm_get_physical(vmm_get_kernel_directory(), (uint64_t)addr);
        vmm_unmap_page(vmm_get_kernel_directory(), (uint64_t)addr);
#endif
        if (phys)
            pmm_free_page((void *)(uintptr_t)phys);
        heap_mapped_end -= PAGE_SIZE;
    }
}

void heap_init(void) {
    heap_free_list = NULL;
    heap_mapped_end = HEAP_START;
    heap_brk = HEAP_START;
    heap_map_until(HEAP_START + HEAP_INITIAL_PAGES * PAGE_SIZE);
    log_printf(LOG_LEVEL_INFO, "heap: initialized at 0x%lx\r\n", (unsigned long)HEAP_START);
}

void vmm_init_heap(void) {
    heap_init();
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);
    uintptr_t need = HEAP_HEADER_SIZE + HEAP_ALIGN(size);
    if (need < HEAP_MIN_BLOCK) need = HEAP_MIN_BLOCK;
    heap_block_t *prev = NULL;
    heap_block_t *b = heap_free_list;
    for (uintptr_t steps = 0; b && steps < HEAP_MAX_WALK; steps++) {
        uintptr_t baddr = (uintptr_t)b;
        if (baddr < HEAP_START || baddr + HEAP_HEADER_SIZE > heap_brk ||
            (baddr & (HEAP_BLOCK_ALIGN - 1)))
            break;
        uintptr_t raw = b->size;
        if (!(raw & HEAP_BLOCK_FREE)) break;
        uintptr_t block_size = raw & HEAP_SIZE_MASK;
        if (block_size < HEAP_MIN_BLOCK || (block_size & (HEAP_BLOCK_ALIGN - 1)) ||
            block_size > heap_brk - baddr)
            break;
        heap_block_t *bnext = b->next;
        if (bnext && ((uintptr_t)bnext < HEAP_START ||
                      (uintptr_t)bnext > heap_brk ||
                      ((uintptr_t)bnext & (HEAP_BLOCK_ALIGN - 1)) ||
                      (uintptr_t)bnext <= baddr))
            break;
        if (block_size >= need) {
            uintptr_t remaining = block_size - need;
            if (remaining >= HEAP_MIN_BLOCK) {
                heap_block_t *split = (heap_block_t *)((uint8_t *)b + need);
                split->size = remaining | HEAP_BLOCK_FREE;
                split->next = bnext;
                if (prev) prev->next = split;
                else heap_free_list = split;
                b->size = need;
            } else {
                if (prev) prev->next = bnext;
                else heap_free_list = bnext;
                b->size = block_size;
            }
            b->magic = HEAP_MAGIC_USED;
            spin_unlock_irqrestore(&heap_lock, flags);
            return (void *)((uint8_t *)b + HEAP_HEADER_SIZE);
        }
        prev = b;
        b = bnext;
    }
    uintptr_t addr = heap_brk;
    uintptr_t new_brk = addr + need;
    if (new_brk < addr || new_brk > HEAP_END) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    if (heap_map_until(new_brk) < 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    heap_brk = new_brk;
    heap_block_t *block = (heap_block_t *)addr;
    block->size = need;
    block->magic = HEAP_MAGIC_USED;
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)((uint8_t *)block + HEAP_HEADER_SIZE);
}

void *kcalloc(uint32_t count, uint32_t size) {
    if (count != 0 && size != 0 && size > 0xFFFFFFFFu / count)
        return NULL;
    uint32_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (uint32_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

void kfree(void *addr) {
    if (!addr) return;
    uintptr_t paddr = (uintptr_t)addr;
    if (paddr < HEAP_START + HEAP_HEADER_SIZE || paddr >= heap_brk ||
        (paddr & (HEAP_BLOCK_ALIGN - 1)) != (HEAP_HEADER_SIZE & (HEAP_BLOCK_ALIGN - 1))) {
        log_printf(LOG_LEVEL_ERROR, "kfree: wild pointer %p\r\n", addr);
        return;
    }
    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);
    heap_block_t *b = (heap_block_t *)((uint8_t *)addr - HEAP_HEADER_SIZE);
    uintptr_t baddr = (uintptr_t)b;
    if (baddr < HEAP_START || baddr + HEAP_HEADER_SIZE > heap_brk ||
        (baddr & (HEAP_BLOCK_ALIGN - 1))) {
        spin_unlock_irqrestore(&heap_lock, flags);
        log_printf(LOG_LEVEL_ERROR, "kfree: bad header %p\r\n", addr);
        return;
    }
    uintptr_t raw = b->size;
    if ((raw & HEAP_BLOCK_FREE) || b->magic != HEAP_MAGIC_USED) {
        spin_unlock_irqrestore(&heap_lock, flags);
        log_printf(LOG_LEVEL_ERROR, "kfree: double-free or corrupt %p\r\n", addr);
        return;
    }
    uintptr_t block_size = raw & HEAP_SIZE_MASK;
    if (!heap_size_sane(raw, baddr)) {
        spin_unlock_irqrestore(&heap_lock, flags);
        log_printf(LOG_LEVEL_ERROR, "kfree: bad size %p\r\n", addr);
        return;
    }
    heap_block_t *prev = NULL;
    heap_block_t *cur = heap_free_list;
    for (uintptr_t steps = 0; cur && steps < HEAP_MAX_WALK; steps++) {
        uintptr_t caddr = (uintptr_t)cur;
        if (caddr < HEAP_START || caddr + HEAP_HEADER_SIZE > heap_brk ||
            (caddr & (HEAP_BLOCK_ALIGN - 1)))
            break;
        if (caddr == baddr) {
            spin_unlock_irqrestore(&heap_lock, flags);
            log_printf(LOG_LEVEL_ERROR, "kfree: double-free %p\r\n", addr);
            return;
        }
        if (caddr > baddr) break;
        prev = cur;
        cur = cur->next;
    }
    b->size = block_size | HEAP_BLOCK_FREE;
    b->next = cur;
    if (prev) prev->next = b;
    else heap_free_list = b;
    heap_coalesce(b);
    if (prev && (uintptr_t)prev + (prev->size & HEAP_SIZE_MASK) == baddr)
        heap_coalesce(prev);
    heap_shrink();
    spin_unlock_irqrestore(&heap_lock, flags);
}

#endif
