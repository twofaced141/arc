#include <stddef.h>
#include "fs.h"
#include "multiboot2.h"
#include "debug.h"

static file_t files[FS_MAX_FILES];
static int file_count;
static ext2_fs_t *ext2_fs;

void fs_set_ext2(ext2_fs_t *fs) {
    ext2_fs = fs;
}

ext2_fs_t *fs_get_ext2(void) {
    return ext2_fs;
}

void fs_init(void *mboot_info) {
    file_count = 0;
    ext2_fs = NULL;

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
    }

    debug_printf("fs: %d file(s) total\r\n", file_count);
}

int fs_count(void) {
    return file_count;
}

file_t *fs_get(int index) {
    if (index < 0 || index >= file_count)
        return NULL;
    return &files[index];
}

file_t *fs_open(const char *name, uint32_t cwd_inode) {
    for (int i = 0; i < file_count; i++) {
        const char *a = name;
        const char *b = files[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return &files[i];
    }

    if (ext2_fs && ext2_fs->present) {
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(ext2_fs, cwd_inode, name, &ino, &type) == 0 && type == EXT2_FT_REG_FILE) {
            ext2_inode_t inode;
            if (ext2_read_inode(ext2_fs, ino, &inode) == 0) {
                for (int i = 0; i < FS_MAX_FILES; i++) {
                    if (files[i].ext2_ino == 0 && files[i].phys_start == 0) {
                        int j;
                        for (j = 0; j < FS_MAX_NAME - 1 && name[j]; j++)
                            files[i].name[j] = name[j];
                        files[i].name[j] = '\0';
                        files[i].size = inode.size;
                        files[i].phys_start = 0;
                        files[i].ext2_ino = ino;
                        return &files[i];
                    }
                }
            }
        }
    }
    return NULL;
}

void fs_read(file_t *f, void *buf, uint32_t offset, uint32_t size) {
    if (!f || offset >= f->size)
        return;
    if (offset + size > f->size)
        size = f->size - offset;

    if (f->ext2_ino && ext2_fs) {
        ext2_read_file(ext2_fs, f->ext2_ino, buf, offset, size);
    } else {
        uint8_t *src = (uint8_t *)(uint32_t)f->phys_start + offset;
        for (uint32_t i = 0; i < size; i++)
            ((uint8_t *)buf)[i] = src[i];
    }
}
