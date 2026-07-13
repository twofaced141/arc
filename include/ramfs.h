#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>

#define RAMFS_MAX_FILES 32
#define RAMFS_MAX_NAME  64

typedef struct ramfs_node {
    char     name[RAMFS_MAX_NAME];
    uint8_t *data;
    uint32_t size;
    int      is_dir;
    struct ramfs_node *next;
} ramfs_node_t;

void ramfs_init(void);
ramfs_node_t *ramfs_add_file(const char *path, const uint8_t *data, uint32_t size);
ramfs_node_t *ramfs_add_dir(const char *path);
ramfs_node_t *ramfs_lookup(const char *path);
void *ramfs_get_file(const char *path, uint32_t *out_size);

#endif
