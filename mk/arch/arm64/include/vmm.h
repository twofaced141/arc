#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "memory.h"
#include "registers.h"

#define VMM_PRESENT      (1 << 0)
#define VMM_WRITABLE     (1 << 1)
#define VMM_USER         (1 << 2)
#define VMM_WRITE_THROUGH (1 << 3)
#define VMM_CACHE_DISABLE (1 << 4)
#define VMM_ACCESSED     (1 << 5)
#define VMM_DIRTY        (1 << 6)

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITE    (1 << 1)
#define VMM_FLAG_USER     (1 << 2)
#define VMM_FLAG_RESERVED (1 << 3)
#define VMM_FLAG_FETCH    (1 << 4)
#define VMM_COW           (1 << 9)

typedef struct page_directory {
    uint64_t entries[512];
} __attribute__((aligned(4096))) page_directory_t;

typedef void (*page_fault_handler_t)(registers_t *r, uint64_t fault_addr,
                                     uint32_t error_code);

void vmm_init(void);
void vmm_init_heap(void);

page_directory_t *vmm_create_directory(void);
void vmm_switch_directory(page_directory_t *dir);
void vmm_free_directory(page_directory_t *dir);
page_directory_t *vmm_get_current_directory(void);
page_directory_t *vmm_get_kernel_directory(void);

int vmm_map_page(page_directory_t *dir, uint64_t phys, uint64_t virt, uint32_t flags);
void vmm_unmap_page(page_directory_t *dir, uint64_t virt);
uint64_t vmm_get_physical(page_directory_t *dir, uint64_t virt);
int vmm_get_page_flags(page_directory_t *dir, uint64_t virt);
int vmm_is_page_present(page_directory_t *dir, uint64_t virt);

void vmm_register_fault_handler(page_fault_handler_t handler);
void vmm_fork_cow_pages(page_directory_t *parent_dir, page_directory_t *child_dir);
void vmm_clear_user_pages(page_directory_t *dir);

void *kmalloc(uint32_t size);
void *kcalloc(uint32_t count, uint32_t size);
void  kfree(void *addr);

#define TEMP_VADDR 0x00007FFFFFFFF000ULL
void *vmm_temp_map(uint64_t phys);
void  vmm_temp_unmap(void);

int copy_from_user(void *dst, const void *user_src, uint32_t size);
int copy_to_user(void *user_dst, const void *src, uint32_t size);
int strncpy_from_user(char *dst, const char *user_src, uint32_t max_len);

int vmm_handle_page_fault(registers_t *r, uint64_t fault_addr, uint32_t esr);

/* Map PCI ECAM config space (from FDT-discovered base) at ECAM_VADDR. */
int vmm_map_pci_ecam(void);

#endif