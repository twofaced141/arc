#include <stdint.h>
#include <stddef.h>
#include "string.h"
#include "debug.h"
#include "bsd/block.h"
#include "bsd/part.h"

#define MAX_PARTITIONS 16

/* ---- Global partition table ---- */
static part_device_t partitions[MAX_PARTITIONS];
static int part_count;

/* ---- Forward declaration of partition block device operations ---- */
static int part_block_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count);
static int part_block_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count);

/* ================================================================
 * MBR parser
 * ================================================================ */

/* Parse MBR partition table and register partitions.
 * Returns number of partitions found, 0 if none, -1 if GPT detected. */
static int mbr_parse(block_dev_t *parent) {
    mbr_t mbr;
    if (blk_read(parent, 0, &mbr, 1) != 0) {
        log_printf(LOG_LEVEL_ERROR, "part: mbr read failed on %s\n", parent->name);
        return 0;
    }

    if (mbr.signature != MBR_SIGNATURE) {
        /* No MBR signature — raw filesystem disk, skip */
        return 0;
    }

    int found = 0;
    for (int i = 0; i < 4; i++) {
        mbr_entry_t *e = &mbr.partitions[i];

        if (e->type == MBR_TYPE_NONE || e->sector_count == 0)
            continue;

        if (e->type == MBR_TYPE_GPT) {
            /* Protective MBR — disk uses GPT, not MBR */
            log_printf(LOG_LEVEL_DEBUG, "part: %s: protective MBR detected (GPT)\n", parent->name);
            /* Don't parse GPT here — gpt_parse() will be called separately */
            return -1; /* signal GPT */
        }

        if (part_count >= MAX_PARTITIONS) {
            log_print(LOG_LEVEL_WARN, "part: too many partitions\n");
            break;
        }

        /* Extended partition — not yet supported */
        if (e->type == MBR_TYPE_EXTENDED) {
            log_printf(LOG_LEVEL_DEBUG, "part: %s: extended partition at LBA %u, skipping\n",
                         parent->name, e->lba_start);
            continue;
        }

        part_device_t *pd = &partitions[part_count];
        pd->parent = parent;
        pd->lba_start = e->lba_start;
        pd->sector_count = e->sector_count;

        int n = strlen(parent->name);
        memcpy(pd->dev.name, parent->name, n);
        pd->dev.name[n] = 'p';
        /* Convert partition number (1-based) — e.g. "ahci0p1", "ahci0p14" */
        int pnum = i + 1;
        int ni = 0;
        if (pnum >= 10) { pd->dev.name[n + 1] = '0' + (pnum / 10); ni++; }
        pd->dev.name[n + 1 + ni] = '0' + (pnum % 10);
        pd->dev.name[n + 2 + ni] = '\0';

        pd->dev.block_size = parent->block_size;
        pd->dev.num_blocks = e->sector_count;
        pd->dev.read  = part_block_read;
        pd->dev.write = part_block_write;
        pd->dev.priv  = pd;

        log_printf(LOG_LEVEL_INFO, "part: %s: MBR partition %d type=0x%02x LBA=%u count=%u -> %s\n",
                     parent->name, i + 1, e->type,
                     e->lba_start, e->sector_count, pd->dev.name);

        part_count++;
        found++;
    }

    return found;
}

/* ================================================================
 * GPT parser
 * ================================================================ */

static int gpt_parse(block_dev_t *parent) {
    uint8_t buf[512];
    gpt_header_t *hdr = (gpt_header_t *)buf;

    /* Read GPT header at LBA 1 */
    if (blk_read(parent, 1, buf, 1) != 0) {
        log_printf(LOG_LEVEL_ERROR, "part: gpt header read failed on %s\n", parent->name);
        return 0;
    }

    if (hdr->signature != GPT_SIGNATURE) {
        /* Not a GPT disk */
        return 0;
    }

    if (hdr->revision != 0x00010000) {
        log_printf(LOG_LEVEL_WARN, "part: %s: GPT revision 0x%x unsupported\n",
                     parent->name, hdr->revision);
        return 0;
    }

    if (hdr->partition_entry_size != 128) {
        log_printf(LOG_LEVEL_WARN, "part: %s: GPT entry size %u != 128\n",
                     parent->name, hdr->partition_entry_size);
        return 0;
    }

    log_printf(LOG_LEVEL_DEBUG, "part: %s: GPT header valid, %u entries at LBA %lu\n",
                 parent->name,
                 hdr->num_partition_entries,
                 (unsigned long)hdr->partition_entry_lba);

    /* Calculate how many sectors we need to read for the partition entries */
    uint32_t entries_per_sector = 512 / hdr->partition_entry_size;
    uint32_t total_sectors = (hdr->num_partition_entries + entries_per_sector - 1)
                           / entries_per_sector;

    /* Allocate a temp buffer for all partition entries */
    size_t entries_size = total_sectors * 512;
    /* Stack allocation for up to 4K; for larger tables we'd need kmalloc */
    #define MAX_ENTRIES_BUF 4096
    uint8_t entries_buf[MAX_ENTRIES_BUF];
    if (entries_size > MAX_ENTRIES_BUF) {
        log_printf(LOG_LEVEL_ERROR, "part: %s: too many GPT entries (%u, need %u bytes)\n",
                     parent->name, hdr->num_partition_entries, (unsigned)entries_size);
        return 0;
    }

    if (blk_read(parent, hdr->partition_entry_lba, entries_buf, total_sectors) != 0) {
        log_printf(LOG_LEVEL_ERROR, "part: %s: failed to read GPT partition entries\n", parent->name);
        return 0;
    }

    int found = 0;
    for (uint32_t i = 0; i < hdr->num_partition_entries; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(entries_buf + i * hdr->partition_entry_size);

        /* Check if entry is unused (all zeros) */
        uint8_t zero[16] = {0};
        if (memcmp(e->type_guid, zero, 16) == 0)
            continue;

        if (part_count >= MAX_PARTITIONS) {
            log_print(LOG_LEVEL_WARN, "part: too many partitions\n");
            break;
        }

        part_device_t *pd = &partitions[part_count];
        pd->parent = parent;
        pd->lba_start = e->first_lba;
        pd->sector_count = e->last_lba - e->first_lba + 1;

        /* Name: parent_name + p + index (e.g., "ahci0p1") */
        int n = strlen(parent->name);
        memcpy(pd->dev.name, parent->name, n);
        pd->dev.name[n] = 'p';
        int ni = 0;
        int pnum = found + 1;
        if (pnum >= 10) { pd->dev.name[n + 1] = '0' + (pnum / 10); ni++; }
        pd->dev.name[n + 1 + ni] = '0' + (pnum % 10);
        pd->dev.name[n + 2 + ni] = '\0';

        pd->dev.block_size = parent->block_size;
        pd->dev.num_blocks = pd->sector_count;
        pd->dev.read  = part_block_read;
        pd->dev.write = part_block_write;
        pd->dev.priv  = pd;

        /* Try to extract partition name (UTF-16LE, convert ASCII chars) */
        char pname[37];
        for (int j = 0; j < 36; j++) {
            uint16_t ch = e->name[j];
            if (ch == 0) { pname[j] = '\0'; break; }
            pname[j] = (ch <= 0x7F) ? (char)ch : '?';
            pname[j + 1] = '\0';
        }

        log_printf(LOG_LEVEL_INFO, "part: %s: GPT partition %u LBA=%lu-%lu count=%lu \"%s\" -> %s\n",
                     parent->name, i + 1,
                     (unsigned long)e->first_lba,
                     (unsigned long)e->last_lba,
                     (unsigned long)pd->sector_count,
                     pname, pd->dev.name);

        part_count++;
        found++;
    }

    return found;
}

/* ================================================================
 * Partition block device operations
 *
 * Translate partition LBA to parent LBA by adding lba_start.
 * Clamp reads/writes to the partition boundary.
 * ================================================================ */

static part_device_t *dev_to_part(block_dev_t *dev) {
    return (part_device_t *)dev->priv;
}

static int part_block_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    part_device_t *pd = dev_to_part(dev);

    if (lba >= pd->sector_count)
        return -1;
    if (lba + count > pd->sector_count)
        count = (size_t)(pd->sector_count - lba);
    if (count == 0)
        return 0;

    return blk_read(pd->parent, pd->lba_start + lba, buf, count);
}

static int part_block_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    part_device_t *pd = dev_to_part(dev);

    if (lba >= pd->sector_count)
        return -1;
    if (lba + count > pd->sector_count)
        count = (size_t)(pd->sector_count - lba);
    if (count == 0)
        return 0;

    return blk_write(pd->parent, pd->lba_start + lba, buf, count);
}

/* ================================================================
 * Public API
 * ================================================================ */

void part_init(void) {
    part_count = 0;
    memset(partitions, 0, sizeof(partitions));

    int total_devs = block_dev_get_count();
    log_printf(LOG_LEVEL_DEBUG, "part: scanning %d block device(s)\n", total_devs);

    for (int i = 0; i < total_devs; i++) {
        block_dev_t *bd = block_dev_get(i);
        if (!bd) continue;

        /* Try MBR first */
        int ret = mbr_parse(bd);
        if (ret > 0) {
            /* MBR partitions found */
            continue;
        }
        if (ret == -1) {
            /* Protective MBR — try GPT */
            ret = gpt_parse(bd);
            if (ret > 0) {
                /* GPT partitions found */
                continue;
            }
            /* GPT failed — fall through to raw */
        }

        /* No MBR/GPT or GPT failed: register the raw device as-is */
        log_printf(LOG_LEVEL_DEBUG, "part: %s: no partition table, leaving as raw\n", bd->name);
    }

    /* Register all partition devices */
    for (int i = 0; i < part_count; i++) {
        block_dev_register(&partitions[i].dev);
    }

    if (part_count > 0) {
        log_printf(LOG_LEVEL_INFO, "part: registered %d partition(s)\n", part_count);
    }
}

int part_get_count(void) {
    return part_count;
}

block_dev_t *part_lookup(const char *name) {
    for (int i = 0; i < part_count; i++) {
        if (strcmp(partitions[i].dev.name, name) == 0)
            return &partitions[i].dev;
    }
    return NULL;
}
