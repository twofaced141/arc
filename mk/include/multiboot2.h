#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_MAGIC 0x36D76289

#define MULTIBOOT_TAG_END       0
#define MULTIBOOT_TAG_CMDLINE   1
#define MULTIBOOT_TAG_MODULE    3
#define MULTIBOOT_TAG_MMAP      6
#define MULTIBOOT_TAG_FRAMEBUFFER 8
#define MULTIBOOT_TAG_ACPI_OLD   14
#define MULTIBOOT_TAG_ACPI_NEW   15

#define MULTIBOOT_MEMORY_AVAILABLE        1
#define MULTIBOOT_MEMORY_RESERVED         2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS              4
#define MULTIBOOT_MEMORY_BADRAM           5

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} multiboot2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} multiboot2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[0];
} multiboot2_tag_module_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    uint8_t  entries[0];
} multiboot2_tag_mmap_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;
    uint8_t  reserved;
} __attribute__((packed)) multiboot2_tag_framebuffer_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint8_t  rsdp[0];
} __attribute__((packed)) multiboot2_tag_acpi_t;

static inline multiboot2_tag_t *multiboot2_first_tag(multiboot2_info_t *info) {
    return (multiboot2_tag_t *)((uint8_t *)info + 8);
}

static inline multiboot2_tag_t *multiboot2_next_tag(multiboot2_tag_t *tag) {
    uint32_t size = (tag->size + 7) & ~7;
    return (multiboot2_tag_t *)((uint8_t *)tag + size);
}

#endif
