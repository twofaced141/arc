#include <stdint.h>
#include <stddef.h>
#include <debug.h>
#include <arc/boot.h>

void arc_boot_dump(const struct arc_boot_info *info)
{
    if (!info) {
        log_print(LOG_LEVEL_ERROR, "boot: info=NULL\n");
        return;
    }

    if ((info->flags & ARC_BOOT_HAS_CMDLINE) && info->cmdline)
        log_printf(LOG_LEVEL_INFO, "boot: cmdline=\"%s\"\n", info->cmdline);

    if (log_get_level() < LOG_LEVEL_DEBUG)
        return;

    debug_print("boot: magic=0x");
#if defined(__x86_64__) || defined(__aarch64__)
    debug_print_hex64(info->magic);
#else
    debug_print_hex32((uint32_t)(info->magic >> 32));
    debug_print_hex32((uint32_t)info->magic);
#endif
    debug_print(" version=");
    debug_print_dec(info->version);
    debug_print(" flags=0x");
    debug_print_hex32(info->flags);

    debug_print("\nboot: memory_entries=");
    debug_print_dec(info->memory_entries);
    debug_print("\n");

    for (size_t i = 0; i < info->memory_entries; i++) {
        const struct arc_memory_region *r = &info->memory_map[i];
        debug_print("  [");
        debug_print_dec(i);
        debug_print("] base=0x");
#if defined(__x86_64__) || defined(__aarch64__)
        debug_print_hex64(r->base);
#else
        debug_print_hex32((uint32_t)(r->base >> 32));
        debug_print_hex32((uint32_t)r->base);
#endif
        debug_print(" len=0x");
#if defined(__x86_64__) || defined(__aarch64__)
        debug_print_hex64(r->length);
#else
        debug_print_hex32((uint32_t)(r->length >> 32));
        debug_print_hex32((uint32_t)r->length);
#endif
        debug_print(" type=");
        debug_print_dec(r->type);
        debug_print("\n");
    }

    if (info->flags & ARC_BOOT_HAS_FB) {
        debug_print("boot: fb addr=0x");
#if defined(__x86_64__) || defined(__aarch64__)
        debug_print_hex64(info->framebuffer.address);
#else
        debug_print_hex32((uint32_t)(info->framebuffer.address >> 32));
        debug_print_hex32((uint32_t)info->framebuffer.address);
#endif
        debug_print(" ");
        debug_print_dec(info->framebuffer.width);
        debug_print("x");
        debug_print_dec(info->framebuffer.height);
        debug_print(" pitch=");
        debug_print_dec(info->framebuffer.pitch);
        debug_print("\n");
    }

    if (info->flags & ARC_BOOT_HAS_ACPI) {
        debug_print("boot: acpi_rsdp=0x");
#if defined(__x86_64__) || defined(__aarch64__)
        debug_print_hex64((uint64_t)(uintptr_t)info->acpi_rsdp);
#else
        debug_print_hex32((uint32_t)(uintptr_t)info->acpi_rsdp);
#endif
        debug_print("\n");
    }

    debug_print("boot: dump done\n");
}
