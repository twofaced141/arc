#include "pmm.h"
#include "terminal.h"
#include "debug.h"
#include "vmm.h"

#define MAX_PAGES (1024 * 1024)

static uint8_t bitmap[MAX_PAGES / 8];
static uint32_t total_pages;
static uint32_t free_pages;
static uint16_t *refcounts;

extern uint32_t _kernel_end;

static inline void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

void pmm_init(multiboot2_info_t *mboot) {
    uint32_t mem_end = 0;

    multiboot2_tag_t *tag = multiboot2_first_tag(mboot);
    while (tag->type != MULTIBOOT_TAG_END) {
        if (tag->type == MULTIBOOT_TAG_MMAP) {
            multiboot2_tag_mmap_t *mtag = (multiboot2_tag_mmap_t *)tag;
            uint8_t *end = (uint8_t *)tag + tag->size;

            for (uint8_t *p = (uint8_t *)mtag->entries;
                 p < end;
                 p += mtag->entry_size) {
                multiboot2_mmap_entry_t *e = (multiboot2_mmap_entry_t *)p;
                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    uint64_t top = e->addr + e->len;
                    if (top > mem_end && top < 0xFFFFFFFF)
                        mem_end = (uint32_t)top;
                }
            }
        }
        tag = multiboot2_next_tag(tag);
    }

    total_pages = mem_end / PAGE_SIZE;
    if (total_pages > MAX_PAGES)
        total_pages = MAX_PAGES;

    for (uint32_t i = 0; i < total_pages / 8 + 1; i++)
        bitmap[i] = 0xFF;

    tag = multiboot2_first_tag(mboot);
    while (tag->type != MULTIBOOT_TAG_END) {
        if (tag->type == MULTIBOOT_TAG_MMAP) {
            multiboot2_tag_mmap_t *mtag = (multiboot2_tag_mmap_t *)tag;
            uint8_t *end = (uint8_t *)tag + tag->size;

            for (uint8_t *p = (uint8_t *)mtag->entries;
                 p < end;
                 p += mtag->entry_size) {
                multiboot2_mmap_entry_t *e = (multiboot2_mmap_entry_t *)p;
                if (e->type != MULTIBOOT_MEMORY_AVAILABLE)
                    continue;

                uint32_t start_page = (e->addr + PAGE_SIZE - 1) / PAGE_SIZE;
                uint32_t end_page   = (e->addr + e->len) / PAGE_SIZE;

                if (end_page > total_pages)
                    end_page = total_pages;

                for (uint32_t i = start_page; i < end_page; i++) {
                    bitmap_clear(i);
                    free_pages++;
                }
            }
        }
        tag = multiboot2_next_tag(tag);
    }

    uint32_t kernel_end_page = ((uint32_t)&_kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < kernel_end_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
        }
    }

    debug_print("pmm: kernel_end=0x");
    debug_print_hex32((uint32_t)&_kernel_end);
    debug_print(" kernel_end_page=0x");
    debug_print_hex32(kernel_end_page);
    debug_print(" first free page test: ");
    debug_print(bitmap_test(0x139) ? "USED" : "FREE");
    debug_print("\r\n");

    uint32_t bitmap_phys = (uint32_t)bitmap;
    uint32_t bitmap_size = sizeof(bitmap);
    uint32_t bmap_start = bitmap_phys / PAGE_SIZE;
    uint32_t bmap_end = (bitmap_phys + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = bmap_start; i < bmap_end; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
        }
    }

    /* Reserve multiboot2 module pages */
    for (tag = multiboot2_first_tag(mboot); tag->type != MULTIBOOT_TAG_END; tag = multiboot2_next_tag(tag)) {
        if (tag->type == MULTIBOOT_TAG_MODULE) {
            multiboot2_tag_module_t *mod = (multiboot2_tag_module_t *)tag;
            uint32_t mod_start_page = mod->mod_start / PAGE_SIZE;
            uint32_t mod_end_page = (mod->mod_end + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint32_t i = mod_start_page; i < mod_end_page; i++) {
                if (!bitmap_test(i)) {
                    bitmap_set(i);
                    free_pages--;
                }
            }
        }
    }

    /* Reserve multiboot2 info structure pages */
    uint32_t mboot_start_page = (uint32_t)mboot / PAGE_SIZE;
    uint32_t mboot_end_page = ((uint32_t)mboot + mboot->total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = mboot_start_page; i < mboot_end_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
        }
    }

    debug_print("pmm: bitmap at 0x");
    debug_print_hex32(bitmap_phys);
    debug_print(" size=0x");
    debug_print_hex32(bitmap_size);
    debug_print("\r\n");
}

void *pmm_alloc_page(void) {
    if (free_pages == 0)
        return (void *)0;

    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            if (!bitmap_test(i))
                continue;
            free_pages--;
            if (refcounts)
                refcounts[i] = 1;
            return (void *)(i * PAGE_SIZE);
        }
    }
    return (void *)0;
}

void *pmm_alloc_pages(uint32_t count) {
    if (count == 0 || free_pages < count)
        return NULL;

    for (uint32_t i = 0; i <= total_pages - count; i++) {
        uint32_t j;
        for (j = 0; j < count; j++) {
            if (bitmap_test(i + j))
                break;
        }

        if (j == count) {
            for (uint32_t k = 0; k < count; k++) {
                bitmap_set(i + k);
                free_pages--;
                if (refcounts)
                    refcounts[i + k] = 1;
            }
            return (void *)(i * PAGE_SIZE);
        }
    }
    return NULL;
}

void pmm_free_page(void *page) {
    uint32_t idx = (uint32_t)page / PAGE_SIZE;
    if (idx < total_pages && bitmap_test(idx)) {
        bitmap_clear(idx);
        free_pages++;
    }
}

void pmm_free_pages(void *addr, uint32_t count) {
    uint32_t start = (uint32_t)addr / PAGE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        if (start + i < total_pages && bitmap_test(start + i)) {
            bitmap_clear(start + i);
            free_pages++;
        }
    }
}

uint32_t pmm_get_free_pages(void) {
    return free_pages;
}

uint32_t pmm_get_total_pages(void) {
    return total_pages;
}

void pmm_refcount_init(void) {
    uint32_t rc_bytes = total_pages * sizeof(uint16_t);
    refcounts = (uint16_t *)kcalloc(1, rc_bytes);
    if (!refcounts) {
        debug_print("pmm: refcount table alloc failed\r\n");
        return;
    }
    for (uint32_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i))
            refcounts[i] = 1;
    }
    debug_printf("pmm: refcounts %u bytes via kcalloc\r\n", rc_bytes);
}

void pmm_refcount_inc(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    if (idx < total_pages)
        refcounts[idx]++;
}

void pmm_refcount_dec(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    if (idx >= total_pages) return;
    if (refcounts[idx] > 1) {
        refcounts[idx]--;
    } else {
        refcounts[idx] = 0;
        pmm_free_page((void *)phys);
    }
}
