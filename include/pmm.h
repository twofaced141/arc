#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include "multiboot2.h"

#define PAGE_SIZE 4096

void pmm_init(multiboot2_info_t *mboot);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint32_t count);
void  pmm_free_page(void *page);
void  pmm_free_pages(void *addr, uint32_t count);
uint32_t pmm_get_free_pages(void);

void pmm_refcount_init(void);
void pmm_refcount_inc(uint32_t phys);
void pmm_refcount_dec(uint32_t phys);

#endif
