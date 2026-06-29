#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "memory.h"

#define VMM_PRESENT   (1 << 0)
#define VMM_WRITABLE  (1 << 1)
#define VMM_USER      (1 << 2)
#define VMM_WRITE_THROUGH (1 << 3)
#define VMM_CACHE_DISABLE (1 << 4)
#define VMM_ACCESSED  (1 << 5)
#define VMM_DIRTY     (1 << 6)
#define VMM_GLOBAL    (1 << 8)

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITE    (1 << 1)
#define VMM_FLAG_USER     (1 << 2)
#define VMM_FLAG_RESERVED (1 << 3)
#define VMM_FLAG_FETCH    (1 << 4)

typedef struct {
    uint32_t entries[1024];
} __attribute__((aligned(4096))) page_table_t;

typedef struct {
    uint32_t entries[1024];
} __attribute__((aligned(4096))) page_directory_t;

typedef struct {
    uint32_t present : 1;
    uint32_t write   : 1;
    uint32_t user    : 1;
    uint32_t reserved: 1;
    uint32_t fetch   : 1;
    uint32_t unused  : 27;
} page_fault_info_t;

typedef void (*page_fault_handler_t)(uint32_t fault_addr, uint32_t error_code);

void vmm_init(void);
void vmm_init_heap(void);

page_directory_t *vmm_create_directory(void);
void vmm_switch_directory(page_directory_t *dir);
void vmm_free_directory(page_directory_t *dir);
page_directory_t *vmm_get_current_directory(void);

void vmm_map_page(page_directory_t *dir, uint32_t phys, uint32_t virt, uint32_t flags);
void vmm_unmap_page(page_directory_t *dir, uint32_t virt);
uint32_t vmm_get_physical(page_directory_t *dir, uint32_t virt);
int vmm_get_page_flags(page_directory_t *dir, uint32_t virt);
int vmm_is_page_present(page_directory_t *dir, uint32_t virt);

void vmm_register_fault_handler(page_fault_handler_t handler);

void *kmalloc(uint32_t size);
void *kcalloc(uint32_t count, uint32_t size);
void  kfree(void *addr);

#define TEMP_VADDR 0xFFC00000
void *vmm_temp_map(uint32_t phys);
void  vmm_temp_unmap(void);

#endif
