#ifndef ARC_BOOT_H
#define ARC_BOOT_H

#include <stdint.h>
#include <stddef.h>

/*
 * ARC Boot Info — platform-agnostic boot information structure.
 *
 * The bootloader frontend (multiboot2, FDT, EFI, etc.) fills an
 * arc_boot_info and passes it to the arch entry point.  This keeps
 * the rest of the kernel independent of any particular boot protocol.
 *
 * Versioning
 * ==========
 * Every struct carries an explicit version.  Fields are never removed;
 * new fields are appended and the version bumped.  Consumers check
 * version before accessing fields that may not be present.
 */

#define ARC_BOOT_MAGIC   0x415243424F4F54ULL /* "ARCBOOT" */

/*
 * Bump ARC_BOOT_VERSION when adding fields to arc_boot_info.
 * The version tells early init code which fields are valid.
 */
#define ARC_BOOT_VERSION 1

/* ── Memory region types ────────────────────────────────────────── */

#define ARC_MEM_USABLE   1
#define ARC_MEM_RESERVED 2
#define ARC_MEM_ACPI     3
#define ARC_MEM_MMIO     4

struct arc_memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;       /* ARC_MEM_* */
};

/* ── Boot info flags ────────────────────────────────────────────── */

enum arc_boot_flags {
    ARC_BOOT_HAS_CMDLINE = 1 << 0,
    ARC_BOOT_HAS_FDT     = 1 << 1,
    ARC_BOOT_HAS_ACPI    = 1 << 2,
    ARC_BOOT_HAS_FB      = 1 << 3,
};

/* ── Framebuffer (optional) ─────────────────────────────────────── */

struct arc_framebuffer {
    uint64_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;     /* reserved for pixel-format enum */
};

/* ── Boot info ──────────────────────────────────────────────────── */

struct arc_boot_info {
    uint64_t magic;                        /* ARC_BOOT_MAGIC */

    uint32_t version;                      /* ARC_BOOT_VERSION */
    uint32_t flags;                        /* enum arc_boot_flags */

    /*
     * Kernel command line.  NULL if absent
     * (check ARC_BOOT_HAS_CMDLINE).
     */
    const char *cmdline;

    /*
     * Physical memory map.
     * memory_entries may be 0 (no map provided).
     */
    struct arc_memory_region *memory_map;
    size_t memory_entries;

    /*
     * Platform data.  At most one of fdt / acpi is non-NULL
     * depending on what the boot firmware provided.
     */
    void *fdt;                             /* ARC_BOOT_HAS_FDT  */
    void *acpi_rsdp;                       /* ARC_BOOT_HAS_ACPI */

    /* Optional framebuffer (check ARC_BOOT_HAS_FB). */
    struct arc_framebuffer framebuffer;
};

/* ── Boot layer API ─────────────────────────────────────────────── */

struct arc_boot_info *arc_boot_init(uint32_t mboot_magic,
                                    void *mboot);
struct arc_boot_info *arc_boot_init_fdt(const void *dtb);
void *arc_boot_raw_info(void);
int  arc_boot_validate(const struct arc_boot_info *info);
void arc_boot_dump(const struct arc_boot_info *info);

#endif /* ARC_BOOT_H */
