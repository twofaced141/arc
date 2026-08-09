#include <stddef.h>
#include <arc/boot.h>

int arc_boot_validate(const struct arc_boot_info *info)
{
    if (!info)
        return -1;

    if (info->magic != ARC_BOOT_MAGIC)
        return -1;

    if (info->version == 0 || info->version > ARC_BOOT_VERSION)
        return -1;

    if (info->flags & ARC_BOOT_HAS_CMDLINE) {
        if (!info->cmdline)
            return -1;
    }

    if (info->memory_entries > 0 && !info->memory_map)
        return -1;

    for (size_t i = 0; i < info->memory_entries; i++) {
        const struct arc_memory_region *r = &info->memory_map[i];
        if (r->length == 0)
            return -1;
        if (r->type != ARC_MEM_USABLE &&
            r->type != ARC_MEM_RESERVED &&
            r->type != ARC_MEM_ACPI &&
            r->type != ARC_MEM_MMIO)
            return -1;
    }

    return 0;
}
