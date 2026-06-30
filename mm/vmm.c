#include "vmm.h"
#include "terminal.h"
#include "debug.h"
#include "panic.h"
#include "spinlock.h"
#include "process.h"
#include "scheduler.h"

static page_directory_t *kernel_directory;
static page_directory_t *current_directory;

static page_fault_handler_t fault_handler;

static inline void invlpg(uint32_t addr) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void switch_cr3(uint32_t dir) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(dir) : "memory");
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

page_directory_t *vmm_get_kernel_directory(void) {
    return kernel_directory;
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

        if (dir->entries[i] == kernel_directory->entries[i])
            continue;

        page_table_t *table = (page_table_t *)(dir->entries[i] & ~0xFFF);

        for (int j = 0; j < 1024; j++) {
            if (table->entries[j] & VMM_PRESENT)
                pmm_refcount_dec(table->entries[j] & ~0xFFF);
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

    /* COW: supervisor (ring0) or user write to a present COW page.
     * error_code & 0xB == 0x3 means: present=1, write=1, reserved=0.
     * Matches both ring3 (0x7) and ring0 (0x3) COW faults. */
    if ((error_code & 0xB) == 0x3) {
        uint32_t pd_idx = fault_addr >> 22;
        uint32_t pt_idx = (fault_addr >> 12) & 0x3FF;

        if (current_directory->entries[pd_idx] & VMM_PRESENT) {
            page_table_t *table = (page_table_t *)(current_directory->entries[pd_idx] & ~0xFFF);
            uint32_t pte = table->entries[pt_idx];

            if ((pte & VMM_COW) && !(pte & VMM_WRITABLE)) {
                uint32_t old_phys = pte & ~0xFFF;
                uint32_t new_phys = (uint32_t)pmm_alloc_page();
                if (!new_phys)
                    panic("COW: OOM", r);

                uint8_t *tmp = (uint8_t *)kmalloc(4096);
                if (!tmp)
                    panic("COW: kmalloc OOM", r);

                uint8_t *src = (uint8_t *)vmm_temp_map(old_phys);
                for (uint32_t i = 0; i < 4096; i++) tmp[i] = src[i];
                vmm_temp_unmap();

                uint8_t *dst = (uint8_t *)vmm_temp_map(new_phys);
                for (uint32_t i = 0; i < 4096; i++) dst[i] = tmp[i];
                vmm_temp_unmap();

                kfree(tmp);

                pmm_refcount_dec(old_phys);

                uint32_t new_flags = (pte & 0xFFF) | VMM_WRITABLE;
                new_flags &= ~VMM_COW;
                table->entries[pt_idx] = (new_phys & ~0xFFF) | new_flags;
                invlpg(fault_addr);

                return;
            }
        }
    }

    if (fault_handler) {
        fault_handler(fault_addr, error_code);
        return;
    }

    /* SIGSEGV: unhandled page fault in user mode */
    if (error_code & 0x4) {
        process_t *cur = scheduler_current_process();
        if (cur) {
            debug_print("SIGSEGV: pid=");
            debug_print_hex32(cur->pid);
            debug_print(" addr=");
            debug_print_hex32(fault_addr);
            debug_print(" eip=");
            debug_print_hex32(r->eip);
            debug_print(" err=");
            debug_print_hex32(error_code);
            debug_print("\r\n");
            cur->signal_pending |= (1 << SIGSEGV);
            scheduler_signal_pending = 1;
            return;
        }
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
static spinlock_t heap_lock = SPINLOCK_INIT;

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

    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

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
            spin_unlock_irqrestore(&heap_lock, flags);
            return (void *)((uint8_t *)b + HEAP_HEADER_SIZE);
        }
        prev = b;
        b = b->next;
    }

    uint32_t addr = heap_brk;
    uint32_t new_brk = addr + need;
    if (new_brk >= HEAP_END) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    heap_map_until(new_brk);
    heap_brk = new_brk;

    heap_block_t *block = (heap_block_t *)addr;
    block->size = need | 0;
    block->next = NULL;
    spin_unlock_irqrestore(&heap_lock, flags);
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

    uint32_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

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

    spin_unlock_irqrestore(&heap_lock, flags);
}

void *vmm_temp_map(uint32_t phys) {
    vmm_map_page(kernel_directory, phys, TEMP_VADDR, VMM_PRESENT | VMM_WRITABLE);
    return (void *)TEMP_VADDR;
}

void vmm_temp_unmap(void) {
    vmm_unmap_page(kernel_directory, TEMP_VADDR);
    invlpg(TEMP_VADDR);
}

int copy_from_user(void *dst, const void *user_src, uint32_t size) {
    if (size == 0) return 0;
    uint32_t addr = (uint32_t)user_src;
    if (addr + size < addr) return -1;
    if (addr + size > 0xC0000000) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    uint32_t end_page = (addr + size + PAGE_SIZE - 1) & ~0xFFF;
    for (uint32_t page = addr & ~0xFFF; page < end_page; page += PAGE_SIZE) {
        if (!vmm_is_page_present(dir, page)) return -1;
    }

    for (uint32_t i = 0; i < size; i++)
        ((uint8_t *)dst)[i] = ((const uint8_t *)user_src)[i];
    return 0;
}

int copy_to_user(void *user_dst, const void *src, uint32_t size) {
    if (size == 0) return 0;
    uint32_t addr = (uint32_t)user_dst;
    if (addr + size < addr) return -1;
    if (addr + size > 0xC0000000) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    uint32_t end_page = (addr + size + PAGE_SIZE - 1) & ~0xFFF;
    for (uint32_t page = addr & ~0xFFF; page < end_page; page += PAGE_SIZE) {
        int flags = vmm_get_page_flags(dir, page);
        if (!(flags & VMM_PRESENT)) return -1;
        if (!(flags & VMM_WRITABLE) && !(flags & VMM_COW)) return -1;
    }

    for (uint32_t i = 0; i < size; i++)
        ((uint8_t *)user_dst)[i] = ((const uint8_t *)src)[i];
    return 0;
}

int strncpy_from_user(char *dst, const char *user_src, uint32_t max_len) {
    if (max_len == 0) return -1;
    uint32_t addr = (uint32_t)user_src;
    if (addr >= 0xC0000000) return -1;
    uint32_t avail = 0xC0000000 - addr;
    if (max_len > avail) max_len = avail;
    if (max_len == 0) return -1;

    page_directory_t *dir = vmm_get_current_directory();
    uint32_t end_page = (addr + max_len + PAGE_SIZE - 1) & ~0xFFF;
    for (uint32_t page = addr & ~0xFFF; page < end_page; page += PAGE_SIZE) {
        if (!vmm_is_page_present(dir, page)) return -1;
    }

    for (uint32_t i = 0; i < max_len; i++) {
        char c = ((const char *)user_src)[i];
        dst[i] = c;
        if (c == '\0') return (int)(i + 1);
    }
    return -1;
}

void vmm_fork_cow_pages(page_directory_t *parent_dir, page_directory_t *child_dir) {
    extern page_directory_t *kernel_directory;
    for (int pd_idx = 0; pd_idx < 768; pd_idx++) {
        uint32_t parent_pde = parent_dir->entries[pd_idx];
        if (!(parent_pde & VMM_PRESENT))
            continue;
        if (parent_pde == kernel_directory->entries[pd_idx])
            continue;

        page_table_t *parent_table = (page_table_t *)(parent_pde & ~0xFFF);

        page_table_t *child_table = (page_table_t *)pmm_alloc_page();
        if (!child_table)
            continue;

        for (int pt_idx = 0; pt_idx < 1024; pt_idx++)
            child_table->entries[pt_idx] = 0;

        for (int pt_idx = 0; pt_idx < 1024; pt_idx++) {
            uint32_t pte = parent_table->entries[pt_idx];
            if (!(pte & VMM_PRESENT))
                continue;

            uint32_t phys = pte & ~0xFFF;
            uint32_t flags = pte & 0xFFF;

            flags |= VMM_COW;
            if (flags & VMM_WRITABLE) {
                flags &= ~VMM_WRITABLE;

                parent_table->entries[pt_idx] = phys | flags;

                uint32_t vaddr = (pd_idx << 22) | (pt_idx << 12);
                invlpg(vaddr);
            }

            child_table->entries[pt_idx] = phys | flags;
            pmm_refcount_inc(phys);
        }

        uint32_t pde_flags = parent_pde & 0xFFF;
        child_dir->entries[pd_idx] = ((uint32_t)child_table & ~0xFFF) | pde_flags;
    }
}

void vmm_clear_user_pages(page_directory_t *dir) {
    extern page_directory_t *kernel_directory;
    for (int i = 0; i < 768; i++) {
        uint32_t pde = dir->entries[i];
        if (!(pde & VMM_PRESENT))
            continue;
        if (pde == kernel_directory->entries[i])
            continue;
        page_table_t *table = (page_table_t *)(pde & ~0xFFF);
        for (int j = 0; j < 1024; j++) {
            uint32_t pte = table->entries[j];
            if (pte & VMM_PRESENT)
                pmm_refcount_dec(pte & ~0xFFF);
            table->entries[j] = 0;
        }
        pmm_free_page(table);
        dir->entries[i] = 0;
    }
}

void vmm_init(void) {
    uint32_t esp_val;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(esp_val));
    debug_print("vmm_init esp=0x");
    debug_print_hex32(esp_val);
    debug_print("\r\n");

    kernel_directory = (page_directory_t *)pmm_alloc_page();
    if (!kernel_directory)
        return;

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

    /* Pre-create TEMP_VADDR PDE so vmm_create_directory copies it for all processes */
    {
        uint32_t pd_idx = TEMP_VADDR >> 22;
        if (!(kernel_directory->entries[pd_idx] & VMM_PRESENT)) {
            page_table_t *t = (page_table_t *)pmm_alloc_page();
            for (int i = 0; i < 1024; i++) t->entries[i] = 0;
            kernel_directory->entries[pd_idx] = (uint32_t)t | VMM_PRESENT | VMM_WRITABLE;
        }
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