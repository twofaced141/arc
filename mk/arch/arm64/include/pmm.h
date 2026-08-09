#ifndef PMM_ARM64_H
#define PMM_ARM64_H

#include <stdint.h>

void pmm_init(void);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint32_t count);
void  pmm_free_page(void *page);
void pmm_free_pages(void *addr, uint32_t count);
uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_total_pages(void);

#endif
