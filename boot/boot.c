#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <multiboot2.h>
#include <arc/boot.h>

#define ARC_BOOT_MAX_REGIONS 64
#define ARC_BOOT_CMDLINE_MAX 512

static struct arc_memory_region boot_regions[ARC_BOOT_MAX_REGIONS];
static struct arc_boot_info   boot_info;
static char                   boot_cmdline[ARC_BOOT_CMDLINE_MAX];
static void                  *boot_raw_mboot;

struct arc_boot_info *arc_boot_init(uint32_t mboot_magic,
                                    void *mboot_ptr)
{
    multiboot2_info_t *mboot = (multiboot2_info_t *)mboot_ptr;
    if (mboot_magic != MULTIBOOT2_MAGIC || !mboot)
        return NULL;

    memset(&boot_info, 0, sizeof(boot_info));
    boot_info.magic   = ARC_BOOT_MAGIC;
    boot_info.version = ARC_BOOT_VERSION;
    boot_info.memory_map  = boot_regions;

    size_t nreg = 0;
    multiboot2_tag_t *tag = multiboot2_first_tag(mboot);

    while (tag->type != MULTIBOOT_TAG_END) {
        switch (tag->type) {
        case MULTIBOOT_TAG_CMDLINE: {
            const char *src = (const char *)tag + 8;
            size_t len = strlen(src);
            if (len >= ARC_BOOT_CMDLINE_MAX)
                len = ARC_BOOT_CMDLINE_MAX - 1;
            memcpy(boot_cmdline, src, len);
            boot_cmdline[len] = '\0';
            boot_info.cmdline = boot_cmdline;
            boot_info.flags  |= ARC_BOOT_HAS_CMDLINE;
            break;
        }
        case MULTIBOOT_TAG_MMAP: {
            multiboot2_tag_mmap_t *mtag = (multiboot2_tag_mmap_t *)tag;
            uint8_t *end = (uint8_t *)tag + tag->size;

            for (uint8_t *p = mtag->entries;
                 p + sizeof(multiboot2_mmap_entry_t) <= end && nreg < ARC_BOOT_MAX_REGIONS;
                 p += mtag->entry_size)
            {
                multiboot2_mmap_entry_t *e = (multiboot2_mmap_entry_t *)p;
                boot_regions[nreg].base   = e->addr;
                boot_regions[nreg].length = e->len;

                switch (e->type) {
                case MULTIBOOT_MEMORY_AVAILABLE:
                    boot_regions[nreg].type = ARC_MEM_USABLE;
                    break;
                case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE:
                    boot_regions[nreg].type = ARC_MEM_ACPI;
                    break;
                case MULTIBOOT_MEMORY_NVS:
                case MULTIBOOT_MEMORY_BADRAM:
                case MULTIBOOT_MEMORY_RESERVED:
                default:
                    boot_regions[nreg].type = ARC_MEM_RESERVED;
                    break;
                }
                nreg++;
            }
            break;
        }
        case MULTIBOOT_TAG_FRAMEBUFFER: {
            multiboot2_tag_framebuffer_t *ftag =
                (multiboot2_tag_framebuffer_t *)tag;
            boot_info.framebuffer.address = ftag->fb_addr;
            boot_info.framebuffer.width   = ftag->fb_width;
            boot_info.framebuffer.height  = ftag->fb_height;
            boot_info.framebuffer.pitch   = ftag->fb_pitch;
            boot_info.framebuffer.format  = ftag->fb_type;
            boot_info.flags |= ARC_BOOT_HAS_FB;
            break;
        }
        case MULTIBOOT_TAG_ACPI_OLD:
        case MULTIBOOT_TAG_ACPI_NEW: {
            multiboot2_tag_acpi_t *atag = (multiboot2_tag_acpi_t *)tag;
            boot_info.acpi_rsdp = (void *)atag->rsdp;
            boot_info.flags |= ARC_BOOT_HAS_ACPI;
            break;
        }
        }
        tag = multiboot2_next_tag(tag);
    }

    boot_raw_mboot = mboot;
    boot_info.memory_entries = nreg;
    return &boot_info;
}

void *arc_boot_raw_info(void)
{
    return boot_raw_mboot;
}
