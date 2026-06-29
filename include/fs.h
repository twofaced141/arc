#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>
#include "ext2.h"

#define FS_MAX_FILES  16
#define FS_MAX_NAME   64

typedef struct {
    char     name[FS_MAX_NAME];
    uint32_t size;
    uint32_t phys_start;
    uint32_t ext2_ino;
} file_t;

void    fs_init(void *mboot_info);
int     fs_count(void);
file_t *fs_get(int index);
file_t *fs_open(const char *name, uint32_t cwd_inode);
void    fs_read(file_t *f, void *buf, uint32_t offset, uint32_t size);
void    fs_set_ext2(ext2_fs_t *fs);
ext2_fs_t *fs_get_ext2(void);

#endif
