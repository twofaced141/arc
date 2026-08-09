#ifndef BSD_PART_H
#define BSD_PART_H

#include <stdint.h>
#include <stddef.h>
#include "bsd/block.h"

/* ================================================================
 * part.h — MBR/GPT partition table parser
 *
 * Scans block devices for partition tables and registers each
 * partition as a virtual block device (e.g., ahci0p1, ahci0p2).
 *
 * A raw device without a partition table is left untouched for
 * backward compatibility (direct ext2 mount).
 * ================================================================ */

/* ---- MBR structures ---- */

/* Partition entry (16 bytes, at offset 446 in sector 0) */
typedef struct __attribute__((packed)) {
    uint8_t  status;          /* 0x80 = bootable */
    uint8_t  chs_first[3];    /* CHS address of first sector */
    uint8_t  type;            /* partition type (0x83=Linux, 0xEE=GPT, ...) */
    uint8_t  chs_last[3];     /* CHS address of last sector */
    uint32_t lba_start;       /* LBA of first sector (little-endian) */
    uint32_t sector_count;    /* number of sectors (little-endian) */
} mbr_entry_t;

/* Master boot record (512 bytes, at LBA 0) */
typedef struct __attribute__((packed)) {
    uint8_t       bootcode[446];
    mbr_entry_t   partitions[4];
    uint16_t      signature;  /* 0xAA55 */
} mbr_t;

/* ---- MBR partition type constants ---- */
#define MBR_TYPE_NONE       0x00
#define MBR_TYPE_FAT12      0x01
#define MBR_TYPE_FAT16      0x04
#define MBR_TYPE_FAT16B     0x06
#define MBR_TYPE_NTFS       0x07
#define MBR_TYPE_FAT32      0x0B
#define MBR_TYPE_FAT32L     0x0C
#define MBR_TYPE_EXTENDED   0x05
#define MBR_TYPE_LINUX      0x83
#define MBR_TYPE_LINUX_LVM  0x8E
#define MBR_TYPE_GPT        0xEE   /* protective MBR for GPT */

#define MBR_SIGNATURE       0xAA55

/* ---- GPT structures ---- */

/* GPT header (at LBA 1, 92 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t signature;           /* "EFI PART" (0x5452415020494645) */
    uint32_t revision;            /* 0x00010000 */
    uint32_t header_size;         /* 92 */
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;              /* LBA of this header (1) */
    uint64_t alternate_lba;       /* LBA of backup header */
    uint64_t first_usable_lba;    /* first usable LBA for partitions */
    uint64_t last_usable_lba;     /* last usable LBA */
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba; /* LBA of partition entries array */
    uint32_t num_partition_entries;
    uint32_t partition_entry_size; /* usually 128 */
    uint32_t partition_entries_crc32;
} __attribute__((packed)) gpt_header_t;

#define GPT_SIGNATURE 0x5452415020494645ull  /* "EFI PART" */

/* GPT partition entry (128 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];            /* UTF-16LE partition name */
} gpt_entry_t;

/* ---- Partition device ---- */

/* Wraps a parent block device, offsetting all reads/writes by lba_start
 * and clamping to the partition's sector_count.  Registered as a normal
 * block_dev_t so filesystem code can mount it directly. */
typedef struct part_device {
    block_dev_t  dev;             /* public block device (name = "parentXpY") */
    block_dev_t *parent;          /* underlying device (e.g. ahci0) */
    uint64_t     lba_start;       /* first sector of this partition */
    uint64_t     sector_count;    /* size of this partition in sectors */
    int          valid;           /* entry in use */
} part_device_t;

/* ---- API ---- */

/* Scan all registered block devices for MBR/GPT tables.
 * Called from bsd_init() after AHCI/ATA drivers have registered their disks. */
void part_init(void);

/* Return number of partition devices found */
int part_get_count(void);

/* Look up a partition device by name (e.g. "ahci0p1") */
block_dev_t *part_lookup(const char *name);

#endif /* BSD_PART_H */
