#include <stdint.h>
#include "memory.h"
#include "pmm.h"

extern uint64_t _kernel_end;

#define TOTAL_MEMORY (64 * 1024 * 1024ULL)
#define KERNEL_PHYS_BASE 0x40000000ULL

static uint64_t next_free;
static int ready;

void pmm_init(void) {
    uint64_t kend = (uint64_t)&_kernel_end;
    next_free = (kend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    ready = 1;
}

void *pmm_alloc_pages(uint32_t count) {
    if (!ready) return 0;
    uint64_t addr = next_free;
    uint64_t end = addr + count * PAGE_SIZE;
    if (end > KERNEL_PHYS_BASE + TOTAL_MEMORY) return 0;
    next_free = end;
    return (void *)addr;
}

void *pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_page(void *page) {
    pmm_free_pages(page, 1);
}

void pmm_free_pages(void *addr, uint32_t count) {
    (void)addr;
    (void)count;
}

uint32_t pmm_get_free_pages(void) {
    if (!ready) return 0;
    uint64_t free = (KERNEL_PHYS_BASE + TOTAL_MEMORY) - next_free;
    return (uint32_t)(free / PAGE_SIZE);
}

uint32_t pmm_get_total_pages(void) {
    return (uint32_t)(TOTAL_MEMORY / PAGE_SIZE);
}
