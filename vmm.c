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
        if (kernel_directory->entries[i] & VMM_PRESENT)
            dir->entries[i] = kernel_directory->entries[i];
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

        uint32_t virt_base = (uint32_t)i * 1024 * PAGE_SIZE;
        if (virt_base >= KERNEL_BASE)
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

    debug_print("PAGE FAULT at 0x");
    debug_print_hex32(fault_addr);
    debug_print(" err=0x");
    debug_print_hex32(error_code);
    debug_print(" eip=0x");
    debug_print_hex32(r->eip);
    debug_print("\r\n");

    panic("Page Fault", r);
}

typedef struct heap_block {
    uint32_t size;
    struct heap_block *next;
} heap_block_t;

#define HEAP_BLOCK_FREE    1
#define HEAP_SIZE_MASK    (~1U)
#define HEAP_HEADER_SIZE   sizeof(heap_block_t)
#define HEAP_ALIGNMENT     16
#define HEAP_ALIGN(sz)     (((sz) + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1))
#define HEAP_MIN_BLOCK     (HEAP_ALIGN(HEAP_HEADER_SIZE + HEAP_ALIGNMENT))

static heap_block_t *heap_free_list;
static uint32_t heap_mapped_end;
static uint32_t heap_brk;

static void heap_map_until(uint32_t addr) {
    while (heap_mapped_end < addr) {
        void *phys = pmm_alloc_page();
        if (!phys) break;
        vmm_map_page(kernel_directory, (uint32_t)phys,
                     heap_mapped_end, VMM_PRESENT | VMM_WRITABLE);
        heap_mapped_end += PAGE_SIZE;
    }
}

static void heap_coalesce(heap_block_t *b) {
    heap_block_t *next = (heap_block_t *)((uint8_t *)b + (b->size & HEAP_SIZE_MASK));
    if ((uint32_t)next < heap_mapped_end && (next->size & HEAP_BLOCK_FREE)) {
        b->size = (b->size & HEAP_SIZE_MASK) + (next->size & HEAP_SIZE_MASK);
        b->next = next->next;
    }
}

void vmm_init_heap(void) {
    heap_free_list = NULL;
    heap_mapped_end = HEAP_START;
    heap_brk = HEAP_START;

    heap_map_until(HEAP_START + HEAP_INITIAL_PAGES * PAGE_SIZE);

    debug_print("vmm: heap initialized at 0xD0000000\r\n");
}

void *kmalloc(uint32_t size) {
    if (size == 0)
        return NULL;

    uint32_t need = HEAP_HEADER_SIZE + HEAP_ALIGN(size);
    if (need < HEAP_MIN_BLOCK)
        need = HEAP_MIN_BLOCK;

    heap_block_t *prev = NULL;
    heap_block_t *b = heap_free_list;

    while (b) {
        uint32_t block_size = b->size & HEAP_SIZE_MASK;
        if (block_size >= need) {
            uint32_t remaining = block_size - need;
            if (remaining >= HEAP_MIN_BLOCK) {
                b->size = need | 0;
                heap_block_t *split = (heap_block_t *)((uint8_t *)b + need);
                split->size = remaining | HEAP_BLOCK_FREE;
                split->next = b->next;
                if (prev)
                    prev->next = split;
                else
                    heap_free_list = split;
            } else {
                b->size = block_size | 0;
                if (prev)
                    prev->next = b->next;
                else
                    heap_free_list = b->next;
            }
            return (void *)((uint8_t *)b + HEAP_HEADER_SIZE);
        }
        prev = b;
        b = b->next;
    }

    uint32_t addr = heap_brk;
    uint32_t new_brk = addr + need;
    if (new_brk >= HEAP_END)
        return NULL;

    heap_map_until(new_brk);
    heap_brk = new_brk;

    heap_block_t *block = (heap_block_t *)addr;
    block->size = need | 0;
    block->next = NULL;
    return (void *)((uint8_t *)block + HEAP_HEADER_SIZE);
}

void *kcalloc(uint32_t count, uint32_t size) {
    uint32_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (uint32_t i = 0; i < total; i++)
            p[i] = 0;
    }
    return ptr;
}

void kfree(void *addr) {
    if (!addr)
        return;

    heap_block_t *b = (heap_block_t *)((uint8_t *)addr - HEAP_HEADER_SIZE);
    uint32_t block_size = b->size & HEAP_SIZE_MASK;

    b->size = block_size | HEAP_BLOCK_FREE;

    heap_block_t *prev = NULL;
    heap_block_t *cur = heap_free_list;

    while (cur && (uint32_t)cur < (uint32_t)b) {
        prev = cur;
        cur = cur->next;
    }

    b->next = cur;
    if (prev)
        prev->next = b;
    else
        heap_free_list = b;

    heap_coalesce(b);
    if (prev && (uint32_t)prev + (prev->size & HEAP_SIZE_MASK) == (uint32_t)b)
        heap_coalesce(prev);
}

void *vmm_temp_map(uint32_t phys) {
    debug_print("temp_map phys=0x");
    debug_print_hex32(phys);
    debug_print("\r\n");
    vmm_map_page(kernel_directory, phys, TEMP_VADDR, VMM_PRESENT | VMM_WRITABLE);
    debug_print("temp_map done\r\n");
    return (void *)TEMP_VADDR;
}

void vmm_temp_unmap(void) {
    vmm_unmap_page(kernel_directory, TEMP_VADDR);
    invlpg(TEMP_VADDR);
}

void vmm_init(void) {
    uint32_t esp_val;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(esp_val));
    debug_print("vmm_init esp=0x");
    debug_print_hex32(esp_val);
    debug_print("\r\n");

    kernel_directory = (page_directory_t *)pmm_alloc_page();
    if (!kernel_directory) {
        terminal_print("VMM: failed to alloc kernel page directory\n");
        return;
    }

    for (int i = 0; i < 1024; i++)
        kernel_directory->entries[i] = 0;

    /* Identity map первые 64MB — чтобы page tables были доступны по физическим адресам */
    for (uint32_t virt = 0; virt < 0x04000000; virt += PAGE_SIZE) {
        vmm_map_page(kernel_directory, virt, virt, VMM_PRESENT | VMM_WRITABLE);
    }

    /* Kernel higher half: физ 1MB+ -> виртуальный 0xC0000000+ */
    for (uint32_t offset = 0; offset < 0x400000; offset += PAGE_SIZE) {
        vmm_map_page(kernel_directory, KERNEL_PHYS + offset,
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
    debug_print("vmm: identity map 0-64MB\r\n");
}