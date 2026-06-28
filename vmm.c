#include "vmm.h"
#include "terminal.h"
#include "debug.h"
#include "panic.h"

static page_directory_t *kernel_directory;
static page_directory_t *current_directory;

static page_fault_handler_t fault_handler;

static inline void invlpg(uint32_t addr) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void switch_cr3(uint32_t dir) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(dir));
}

static inline uint32_t read_cr0(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void write_cr0(uint32_t cr0) {
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0));
}

static inline uint32_t read_cr2(void) {
    uint32_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static page_table_t *get_table(page_directory_t *dir, uint32_t pd_index, int create) {
    if (dir->entries[pd_index] & VMM_PRESENT)
        return (page_table_t *)(dir->entries[pd_index] & ~0xFFF);

    if (!create)
        return NULL;

    page_table_t *table = (page_table_t *)pmm_alloc_page();
    if (!table)
        return NULL;

    for (int i = 0; i < 1024; i++)
        table->entries[i] = 0;

    dir->entries[pd_index] = (uint32_t)table | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return table;
}

page_directory_t *vmm_create_directory(void) {
    page_directory_t *dir = (page_directory_t *)pmm_alloc_page();
    if (!dir)
        return NULL;

    for (int i = 0; i < 1024; i++)
        dir->entries[i] = 0;

    for (int i = 0; i < 1024; i++) {
        if (kernel_directory->entries[i] & VMM_PRESENT) {
            dir->entries[i] = kernel_directory->entries[i];
        }
    }

    return dir;
}

void vmm_switch_directory(page_directory_t *dir) {
    current_directory = dir;
    switch_cr3((uint32_t)dir);
}

page_directory_t *vmm_get_current_directory(void) {
    return current_directory;
}

void vmm_free_directory(page_directory_t *dir) {
    if (dir == kernel_directory || dir == current_directory)
        return;

    for (int i = 0; i < 1024; i++) {
        if (!(dir->entries[i] & VMM_PRESENT))
            continue;

        if ((uint32_t)i * 4096 * 1024 >= HEAP_START && (uint32_t)i * 4096 * 1024 < HEAP_END)
            continue;

        page_table_t *table = (page_table_t *)(dir->entries[i] & ~0xFFF);

        for (int j = 0; j < 1024; j++) {
            if (table->entries[j] & VMM_PRESENT) {
                void *page = (void *)(table->entries[j] & ~0xFFF);
                pmm_free_page(page);
            }
        }

        pmm_free_page(table);
    }

    pmm_free_page(dir);
}

void vmm_map_page(page_directory_t *dir, uint32_t phys, uint32_t virt, uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    page_table_t *table = get_table(dir, pd_index, 1);
    if (!table)
        return;

    table->entries[pt_index] = (phys & ~0xFFF) | flags;
    invlpg(virt);
}

void vmm_unmap_page(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    table->entries[pt_index] = 0;
    invlpg(virt);
}

uint32_t vmm_get_physical(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);

    if (!(table->entries[pt_index] & VMM_PRESENT))
        return 0;

    return (table->entries[pt_index] & ~0xFFF) | (virt & 0xFFF);
}

int vmm_get_page_flags(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    return table->entries[pt_index] & 0xFFF;
}

int vmm_is_page_present(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir->entries[pd_index] & VMM_PRESENT))
        return 0;

    page_table_t *table = (page_table_t *)(dir->entries[pd_index] & ~0xFFF);
    return (table->entries[pt_index] & VMM_PRESENT) != 0;
}

void vmm_register_fault_handler(page_fault_handler_t handler) {
    fault_handler = handler;
}

static void page_fault_handler(registers_t *r) {
    uint32_t fault_addr = read_cr2();
    uint32_t error_code = r->err_code;

    if (fault_handler) {
        fault_handler(fault_addr, error_code);
        return;
    }

    terminal_print("\nPAGE FAULT at 0x");

    char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        terminal_putchar(hex[(fault_addr >> i) & 0xF]);

    terminal_print("\nError: ");
    if (error_code & VMM_FLAG_PRESENT) terminal_print("PRESENT ");
    if (error_code & VMM_FLAG_WRITE) terminal_print("WRITE ");
    if (error_code & VMM_FLAG_USER) terminal_print("USER ");
    if (error_code & VMM_FLAG_RESERVED) terminal_print("RESERVED ");
    if (error_code & VMM_FLAG_FETCH) terminal_print("FETCH ");
    terminal_print("\n");

    panic("Page Fault", r);
}

static void *heap_current;
static void *heap_start;
static void *heap_end;

void vmm_init_heap(void) {
    heap_start = (void *)HEAP_START;
    heap_end = (void *)HEAP_END;
    heap_current = heap_start;

    for (uint32_t i = 0; i < HEAP_INITIAL_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (!phys)
            break;
        vmm_map_page(kernel_directory, (uint32_t)phys,
                      HEAP_START + i * PAGE_SIZE,
                      VMM_PRESENT | VMM_WRITABLE);
    }

    debug_print("vmm: heap initialized at 0xD0000000\r\n");
}

static uint32_t heap_sbrk(uint32_t pages) {
    uint32_t allocated = 0;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t addr = (uint32_t)heap_current + allocated;
        if (addr >= HEAP_END)
            break;

        void *phys = pmm_alloc_page();
        if (!phys)
            break;

        vmm_map_page(kernel_directory, (uint32_t)phys, addr,
                      VMM_PRESENT | VMM_WRITABLE);
        allocated += PAGE_SIZE;
    }

    return allocated;
}

void *kmalloc(uint32_t size) {
    if (size == 0)
        return NULL;

    uint32_t aligned = (size + 15) & ~15;
    uint32_t pages_needed = (aligned + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t addr = (uint32_t)heap_current;
    uint32_t heap_used = addr - (uint32_t)heap_start;
    uint32_t heap_total_pages = heap_used / PAGE_SIZE + pages_needed + 1;

    if (heap_total_pages > HEAP_INITIAL_PAGES) {
        uint32_t extra = heap_total_pages - HEAP_INITIAL_PAGES;
        uint32_t allocated = heap_sbrk(extra);
        if (allocated < extra * PAGE_SIZE) {
            return NULL;
        }
    }

    heap_current = (void *)(addr + aligned);
    return (void *)addr;
}

void *kcalloc(uint32_t count, uint32_t size) {
    uint32_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint32_t *p = (uint32_t *)ptr;
        for (uint32_t i = 0; i < total / 4; i++)
            p[i] = 0;
        for (uint32_t i = 0; i < total % 4; i++)
            ((uint8_t *)ptr)[total - 1 - i] = 0;
    }
    return ptr;
}

void kfree(void *addr) {
    (void)addr;
}

void vmm_init(void) {
    kernel_directory = (page_directory_t *)pmm_alloc_page();
    if (!kernel_directory) {
        terminal_print("VMM: failed to alloc kernel page directory\n");
        return;
    }

    for (int i = 0; i < 1024; i++)
        kernel_directory->entries[i] = 0;

    for (uint32_t virt = 0; virt < 0x00400000; virt += PAGE_SIZE) {
        void *phys = (void *)virt;
        vmm_map_page(kernel_directory, (uint32_t)phys, virt,
                      VMM_PRESENT | VMM_WRITABLE);
    }

    uint32_t kernel_phys_start = KERNEL_PHYS;
    uint32_t kernel_size = 0x400000;
    for (uint32_t offset = 0; offset < kernel_size; offset += PAGE_SIZE) {
        vmm_map_page(kernel_directory, kernel_phys_start + offset,
                      KERNEL_BASE + offset,
                      VMM_PRESENT | VMM_WRITABLE);
    }

    current_directory = kernel_directory;
    switch_cr3((uint32_t)kernel_directory);

    uint32_t cr0 = read_cr0();
    cr0 |= (1 << 31);
    write_cr0(cr0);

    register_interrupt_handler(14, page_fault_handler);

    debug_print("vmm: paging enabled\r\n");
    debug_print("vmm: kernel mapped at 0xC0000000 (higher half)\r\n");
    debug_print("vmm: identity map 0-4MB\r\n");
}
