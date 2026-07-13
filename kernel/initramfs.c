#include "ramfs.h"
#include "process.h"
#include "scheduler.h"
#include "debug.h"
#include "fs.h"
#include "pmm.h"
#include <string.h>
#include <stddef.h>

extern char _binary_initramfs_init_elf_start[];
extern char _binary_initramfs_init_elf_end[];

static int initramfs_size(void) {
    return (int)(_binary_initramfs_init_elf_end - _binary_initramfs_init_elf_start);
}

process_t *initramfs_run(void) {
    int size = initramfs_size();
    if (size <= 0) {
        debug_printf("initramfs: not available\n");
        return NULL;
    }
    debug_printf("initramfs: size=%d bytes\n", size);

    ramfs_init();

    uint8_t *data = (uint8_t *)pmm_alloc_pages((size + 4095) / 4096);
    if (!data) {
        debug_printf("initramfs: alloc failed\n");
        return NULL;
    }
    for (int i = 0; i < size; i++)
        data[i] = _binary_initramfs_init_elf_start[i];

    ramfs_add_file("/init", data, size);

    file_t f;
    f.ext2_ino = 0;
    f.ext2_fs = NULL;
    f.fat_fs = NULL;
    f.fat_cluster = 0;
    f.phys_start = (uint32_t)data;
    f.size = size;
    f.name[0] = '/';
    f.name[1] = 'i';
    f.name[2] = 'n';
    f.name[3] = 'i';
    f.name[4] = 't';
    f.name[5] = '\0';
    f.name[4] = 't';
    f.name[5] = '\0';

    process_t *proc = process_create_elf(&f);
    if (!proc) {
        debug_printf("initramfs: process_create_elf failed\n");
        return NULL;
    }

    debug_printf("initramfs: PID %d running /init\n", proc->pid);
    return proc;
}
