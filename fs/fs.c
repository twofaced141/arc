#include <stddef.h>
#include <string.h>
#include "fs.h"
#include "multiboot2.h"
#include "debug.h"
#include "spinlock.h"

static file_t files[FS_MAX_FILES];
static int file_count;
static ext2_fs_t *ext2_fs;
static spinlock_t fs_lock = SPINLOCK_INIT;

typedef struct {
    char mount_point[FS_MAX_NAME * 2];
    ext2_fs_t fs;
    int used;
} fs_mount_entry_t;

static fs_mount_entry_t mounts[FS_MOUNT_MAX];

/* FAT32 filesystem instances (max 2 for now: root + /mnt) */
#define FAT_FS_MAX 2
static fat_fs_t fat_fses[FAT_FS_MAX];
static int fat_fs_count;

void fs_set_ext2(ext2_fs_t *fs) {
    ext2_fs = fs;
}

ext2_fs_t *fs_get_ext2(void) {
    return ext2_fs ? ext2_fs : fs_get_root();
}

int fs_mount(const char *point, block_device_t *dev, uint32_t lba_offset) {
    /* Replace existing mount at same point */
    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].mount_point, point) == 0) {
            mounts[i].used = 0;
            break;
        }
    }
    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (!mounts[i].used) {
            int j;
            for (j = 0; point[j] && j < (int)sizeof(mounts[i].mount_point) - 1; j++)
                mounts[i].mount_point[j] = point[j];
            mounts[i].mount_point[j] = '\0';

            if (ext2_init(&mounts[i].fs, dev, lba_offset) < 0) {
                debug_printf("fs_mount: ext2_init failed for %s\r\n", point);
                return -1;
            }
            mounts[i].used = 1;
            debug_printf("fs: mounted %s\r\n", point);
            return 0;
        }
    }
    return -1;
}

int fs_mount_fat(block_device_t *dev, uint32_t lba_offset) {
    if (fat_fs_count >= FAT_FS_MAX)
        return -1;
    if (fat_init(&fat_fses[fat_fs_count], dev, lba_offset) < 0)
        return -1;
    fat_fs_count++;
    return fat_fs_count - 1;
}

fat_fs_t *fs_get_fat(int index) {
    if (index < 0 || index >= fat_fs_count)
        return NULL;
    return &fat_fses[index];
}

int fs_fat_count(void) {
    return fat_fs_count;
}

ext2_fs_t *fs_for_path(char *path) {
    if (!path || path[0] != '/')
        return ext2_fs ? ext2_fs : fs_get_root();

    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (!mounts[i].used) continue;
        int mlen = strlen(mounts[i].mount_point);
        if (strncmp(path, mounts[i].mount_point, mlen) == 0) {
            if (path[mlen] == '/' || path[mlen] == '\0' || (mlen == 1 && mounts[i].mount_point[0] == '/')) {
                int shift = mlen;
                if (path[mlen] == '/') shift++;
                int rem = strlen(path + shift);
                memmove(path, path + shift, rem + 1);
                if (path[0] == '\0') {
                    path[0] = '/';
                    path[1] = '\0';
                }
                return &mounts[i].fs;
            }
        }
    }
    return ext2_fs ? ext2_fs : fs_get_root();
}

ext2_fs_t *fs_get_root(void) {
    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].mount_point, "/") == 0)
            return &mounts[i].fs;
    }
    return NULL;
}

ext2_fs_t *fs_get_mnt(void) {
    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].mount_point, "/mnt") == 0)
            return &mounts[i].fs;
    }
    return NULL;
}

int fs_mount_count(void) {
    int count = 0;
    for (int i = 0; i < FS_MOUNT_MAX; i++)
        if (mounts[i].used) count++;
    return count;
}

const char *fs_mount_point(int index) {
    int seen = 0;
    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].used) {
            if (seen == index) return mounts[i].mount_point;
            seen++;
        }
    }
    return NULL;
}

int fs_mount_on_root(void) {
    int count = 0;
    for (int i = 0; i < FS_MOUNT_MAX; i++) {
        if (!mounts[i].used) continue;
        /* Skip the root mount */
        if (strcmp(mounts[i].mount_point, "/") == 0) continue;
        count++;
    }
    return count;
}

void fs_init(void *mboot_info) {
    file_count = 0;
    ext2_fs = NULL;
    for (int i = 0; i < FS_MOUNT_MAX; i++)
        mounts[i].used = 0;

    multiboot2_tag_t *tag = multiboot2_first_tag((multiboot2_info_t *)mboot_info);
    for (;;) {
        if (tag->type == MULTIBOOT_TAG_END)
            break;

        if (tag->type == MULTIBOOT_TAG_MODULE && file_count < FS_MAX_FILES) {
            multiboot2_tag_module_t *mod = (multiboot2_tag_module_t *)tag;
            file_t *f = &files[file_count];
            f->phys_start = mod->mod_start;
            f->size = mod->mod_end - mod->mod_start;
            f->ext2_ino = 0;
            f->ext2_fs = NULL;

            const char *cmd = mod->cmdline;
            int i;
            for (i = 0; i < FS_MAX_NAME - 1 && cmd[i]; i++)
                f->name[i] = cmd[i];
            f->name[i] = '\0';

            debug_printf("fs: loaded '%s' (%u bytes)\r\n", f->name, f->size);
            file_count++;
        }

        tag = multiboot2_next_tag(tag);
    }

    for (int i = file_count; i < FS_MAX_FILES; i++) {
        files[i].phys_start = 0;
        files[i].ext2_ino = 0;
        files[i].ext2_fs = NULL;
    }

    debug_printf("fs: %d file(s) total\r\n", file_count);
}

int fs_count(void) {
    uint32_t flags;
    spin_lock_irqsave(&fs_lock, &flags);
    int count = file_count;
    spin_unlock_irqrestore(&fs_lock, flags);
    return count;
}

file_t *fs_get(int index) {
    uint32_t flags;
    spin_lock_irqsave(&fs_lock, &flags);
    if (index < 0 || index >= file_count) {
        spin_unlock_irqrestore(&fs_lock, flags);
        return NULL;
    }
    file_t *f = &files[index];
    spin_unlock_irqrestore(&fs_lock, flags);
    return f;
}

file_t *fs_open(const char *name, uint32_t cwd_inode) {
    uint32_t flags;
    spin_lock_irqsave(&fs_lock, &flags);

    for (int i = 0; i < file_count; i++) {
        const char *a = name;
        const char *b = files[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            spin_unlock_irqrestore(&fs_lock, flags);
            return &files[i];
        }
    }

    /* Resolve mount point */
    ext2_fs_t *target_fs = ext2_fs;
    char local[FS_MAX_NAME * 2];
    int local_pos = 0;
    if (name[0] == '/') {
        for (int j = 0; name[j] && j < (int)sizeof(local) - 1; j++)
            local[j] = name[j];
        local[name[0] ? (int)strlen(name) : 0] = '\0';
        char *lp = local;
        target_fs = fs_for_path(lp);
        for (int j = 0; lp[j] && j < (int)sizeof(local) - 1; j++)
            local[j] = lp[j];
        local_pos = strlen(lp);
        local[local_pos] = '\0';
    } else {
        int j;
        for (j = 0; name[j] && j < (int)sizeof(local) - 1; j++)
            local[j] = name[j];
        local[j] = '\0';
    }

    if (target_fs && target_fs->present) {
        uint32_t ino;
        uint8_t type;
        uint32_t base_ino = name[0] == '/' ? EXT2_ROOT_INO : cwd_inode;
        if (ext2_resolve(target_fs, base_ino, local, &ino, &type) == 0 && type == EXT2_FT_REG_FILE) {
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (files[i].ext2_ino == ino && files[i].ext2_fs == target_fs) {
                    spin_unlock_irqrestore(&fs_lock, flags);
                    return &files[i];
                }
            }
            ext2_inode_t inode;
            if (ext2_read_inode(target_fs, ino, &inode) == 0) {
                for (int i = 0; i < FS_MAX_FILES; i++) {
                    if (files[i].ext2_ino == 0 && files[i].phys_start == 0) {
                        int j;
                        for (j = 0; j < FS_MAX_NAME - 1 && name[j]; j++)
                            files[i].name[j] = name[j];
                        files[i].name[j] = '\0';
                        files[i].size = inode.size;
                        files[i].phys_start = 0;
                        files[i].ext2_ino = ino;
                        files[i].ext2_fs = target_fs;
                        files[i].fat_fs = NULL;
                        files[i].fat_cluster = 0;
                        spin_unlock_irqrestore(&fs_lock, flags);
                        return &files[i];
                    }
                }
            }
        }
    }

    /* Try FAT32 filesystems (path already mount-point-stripped in local) */
    const char *fat_path = local;
    if (fat_path[0] == '\0')
        fat_path = ".";
    for (int fi = 0; fi < fat_fs_count; fi++) {
        fat_fs_t *fat = &fat_fses[fi];
        if (!fat->present) continue;
        uint32_t fc;
        uint32_t fsize;
        uint8_t fattrs;
        if (fat_resolve(fat, fat->root_cluster, fat_path, &fc, &fsize, &fattrs) == 0 &&
            !(fattrs & ATTR_DIRECTORY)) {
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (files[i].ext2_ino == 0 && files[i].phys_start == 0) {
                    int j;
                    for (j = 0; j < FS_MAX_NAME - 1 && name[j]; j++)
                        files[i].name[j] = name[j];
                    files[i].name[j] = '\0';
                    files[i].size = fsize;
                    files[i].phys_start = 0;
                    files[i].ext2_ino = 0;
                    files[i].ext2_fs = NULL;
                    files[i].fat_fs = fat;
                    files[i].fat_cluster = fc;
                    spin_unlock_irqrestore(&fs_lock, flags);
                    return &files[i];
                }
            }
        }
    }

    spin_unlock_irqrestore(&fs_lock, flags);
    return NULL;
}

int fs_write(file_t *f, const void *buf, uint32_t offset, uint32_t size) {
    if (!f) return -1;
    if (f->fat_fs) return -1;  /* FAT read-only for now */
    if (!f->ext2_ino || !f->ext2_fs)
        return -1;
    int ret = ext2_write_file(f->ext2_fs, f->ext2_ino, buf, offset, size);
    if (ret > 0) {
        uint32_t new_size = offset + ret;
        if (new_size > f->size)
            f->size = new_size;
    }
    return ret;
}

int fs_create(const char *name, uint32_t cwd_inode, uint32_t *out_ino) {
    ext2_fs_t *target_fs = ext2_fs;
    uint32_t base_ino = cwd_inode;
    const char *create_name = name;

    if (name[0] == '/') {
        char local[FS_MAX_NAME * 2];
        int j;
        for (j = 0; name[j] && j < (int)sizeof(local) - 1; j++)
            local[j] = name[j];
        local[j] = '\0';
        char *lp = local;
        target_fs = fs_for_path(lp);
        base_ino = EXT2_ROOT_INO;
        create_name = lp;
    }

    if (!target_fs || !target_fs->present)
        return -1;

    uint32_t dir_ino;
    uint8_t type;
    const char *base_name = create_name;

    const char *slash = create_name;
    while (*slash) slash++;
    while (slash > create_name && *slash != '/') slash--;

    if (slash == create_name) {
        dir_ino = base_ino;
        base_name = create_name;
        if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
    } else {
        char dir_path[256];
        int len = slash - create_name;
        for (int i = 0; i < len && i < 255; i++) dir_path[i] = create_name[i];
        dir_path[len] = '\0';
        if (ext2_resolve(target_fs, base_ino, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
            return -1;
        base_name = slash + 1;
    }

    if (!*base_name) return -1;

    return ext2_create(target_fs, base_name, dir_ino, EXT2_S_IFREG | 0644, out_ino);
}

void fs_read(file_t *f, void *buf, uint32_t offset, uint32_t size) {
    if (!f || offset >= f->size)
        return;
    if (offset + size > f->size)
        size = f->size - offset;

    if (f->ext2_ino && f->ext2_fs) {
        ext2_read_file(f->ext2_fs, f->ext2_ino, buf, offset, size);
    } else if (f->fat_fs && f->fat_cluster) {
        fat_read_file(f->fat_fs, f->fat_cluster, f->size, buf, offset, size);
    } else {
        uint8_t *src = (uint8_t *)(uint32_t)f->phys_start + offset;
        for (uint32_t i = 0; i < size; i++)
            ((uint8_t *)buf)[i] = src[i];
    }
}
