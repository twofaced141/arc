#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include "spinlock.h"
#include "block.h"

#define FAT32_MAGIC  0x28
#define FAT32_MAGIC2 0x29

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F

#define FAT_EOF   0x0FFFFFF8
#define FAT_BAD   0x0FFFFFF7
#define FAT_FREE  0x00000000

#define FAT32_ROOT_CLUSTER ((uint32_t)-1)

typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
} __attribute__((packed)) fat_bpb_t;

#define FAT_SIGNATURE 0xAA55

typedef struct {
    char     name[8];
    char     ext[3];
    uint8_t  attrs;
    uint8_t  nt_res;
    uint8_t  tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access;
    uint16_t cluster_hi;
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;

#define FAT_CACHE_SIZE 64

typedef struct {
    uint32_t sector;
    uint8_t  data[512];
    int      valid;
} fat_cache_line_t;

typedef struct {
    fat_bpb_t bpb;
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  fat_start_sector;
    uint32_t  data_start_sector;
    uint32_t  root_cluster;
    uint32_t  total_clusters;
    int       present;
    block_device_t *dev;
    uint32_t  lba_offset;
    fat_cache_line_t fat_cache[FAT_CACHE_SIZE];
    spinlock_t lock;
} fat_fs_t;

int  fat_init(fat_fs_t *fs, block_device_t *dev, uint32_t lba_offset);
int  fat_probe(block_device_t *dev, uint32_t lba_offset);
int  fat_read_cluster(fat_fs_t *fs, uint32_t cluster, void *buf);
uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster);
int  fat_resolve(fat_fs_t *fs, uint32_t dir_cluster, const char *path, uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attrs);
int  fat_read_file(fat_fs_t *fs, uint32_t cluster, uint32_t size, void *buf, uint32_t offset, uint32_t count);
int  fat_read_dir(fat_fs_t *fs, uint32_t dir_cluster, uint32_t *out_ino, uint32_t index, char *name_out, uint32_t *out_size, uint8_t *out_attrs);

#endif
