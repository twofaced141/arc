#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>
#include "ext2.h"
#include "fat.h"
#include "block.h"

#define FS_MAX_FILES  16
#define FS_MAX_NAME   64
#define FS_MOUNT_MAX  8

typedef struct {
    char     name[FS_MAX_NAME];
    uint32_t size;
    uint32_t phys_start;
    uint32_t ext2_ino;
    ext2_fs_t *ext2_fs;
    fat_fs_t *fat_fs;
    uint32_t fat_cluster;
} file_t;

void    fs_init(void *mboot_info);
int     fs_count(void);
file_t *fs_get(int index);
file_t *fs_open(const char *name, uint32_t cwd_inode);
void    fs_read(file_t *f, void *buf, uint32_t offset, uint32_t size);
ext2_fs_t *fs_get_ext2(void);
int     fs_write(file_t *f, const void *buf, uint32_t offset, uint32_t size);
int     fs_create(const char *name, uint32_t cwd_inode, uint32_t *out_ino);

int     fs_mount(const char *point, block_device_t *dev, uint32_t lba_offset);
int     fs_mount_fat(block_device_t *dev, uint32_t lba_offset);
fat_fs_t *fs_get_fat(int index);
int     fs_fat_count(void);
ext2_fs_t *fs_for_path(char *path);
ext2_fs_t *fs_get_mnt(void);
ext2_fs_t *fs_get_root(void);
int     fs_mount_count(void);
const char *fs_mount_point(int index);
int     fs_mount_on_root(void);

#endif
