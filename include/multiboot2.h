#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_MAGIC 0x36d76289

#define MULTIBOOT_TAG_END              0
#define MULTIBOOT_TAG_CMDLINE          1
#define MULTIBOOT_TAG_MODULE           3
#define MULTIBOOT_TAG_BASIC_MEMINFO    4
#define MULTIBOOT_TAG_MMAP             6

#define MULTIBOOT_MEMORY_AVAILABLE     1
#define MULTIBOOT_MEMORY_RESERVED      2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS           4
#define MULTIBOOT_MEMORY_BADRAM        5

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    char     cmdline[];
} __attribute__((packed)) multiboot2_tag_cmdline_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot2_tag_t;

#define MULTIBOOT_TAG_FRAMEBUFFER     8

#define MULTIBOOT_FRAMEBUFFER_TEXT     0
#define MULTIBOOT_FRAMEBUFFER_GRAPHICS 3

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((packed)) multiboot2_tag_basic_meminfo_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[];
} __attribute__((packed)) multiboot2_tag_module_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    multiboot2_mmap_entry_t entries[];
} __attribute__((packed)) multiboot2_tag_mmap_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;
    uint16_t fb_reserved;
} __attribute__((packed)) multiboot2_tag_framebuffer_t;

static inline multiboot2_tag_t *multiboot2_first_tag(multiboot2_info_t *info) {
    return (multiboot2_tag_t *)((uint8_t *)info + 8);
}

static inline multiboot2_tag_t *multiboot2_next_tag(multiboot2_tag_t *tag) {
    uint32_t aligned = (tag->size + 7) & ~7;
    return (multiboot2_tag_t *)((uint8_t *)tag + aligned);
}

#endif
