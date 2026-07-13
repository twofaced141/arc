#include "ramfs.h"
#include "pmm.h"
#include <string.h>
#include <stddef.h>
#include "debug.h"

static ramfs_node_t nodes[RAMFS_MAX_FILES];
static int node_count;

static ramfs_node_t *root_dir;

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

void ramfs_init(void) {
    node_count = 0;
    root_dir = NULL;

    root_dir = ramfs_add_dir("/");
}

static ramfs_node_t *alloc_node(void) {
    if (node_count >= RAMFS_MAX_FILES) return NULL;
    ramfs_node_t *n = &nodes[node_count++];
    n->data = NULL;
    n->size = 0;
    n->is_dir = 0;
    n->next = NULL;
    n->name[0] = '\0';
    return n;
}

ramfs_node_t *ramfs_add_dir(const char *path) {
    ramfs_node_t *n = alloc_node();
    if (!n) return NULL;
    int i;
    for (i = 0; path[i] && i < RAMFS_MAX_NAME - 1; i++)
        n->name[i] = path[i];
    n->name[i] = '\0';
    n->is_dir = 1;

    if (streq(path, "/")) {
        root_dir = n;
    } else {
        /* Link into parent directory's list */
        ramfs_node_t *parent = root_dir;
        ramfs_node_t *p = root_dir->next;
        while (p) { parent = p; p = p->next; }
        parent->next = n;
    }
    return n;
}

ramfs_node_t *ramfs_add_file(const char *path, const uint8_t *data, uint32_t size) {
    ramfs_node_t *n = alloc_node();
    if (!n) return NULL;

    int i;
    for (i = 0; path[i] && i < RAMFS_MAX_NAME - 1; i++)
        n->name[i] = path[i];
    n->name[i] = '\0';

    if (size > 0) {
        uint32_t pages = (size + 4095) / 4096;
        if (pages == 0) pages = 1;
        uint8_t *ram_data = (uint8_t *)pmm_alloc_pages(pages);
        if (!ram_data) return NULL;
        for (uint32_t j = 0; j < size; j++)
            ram_data[j] = data[j];
        n->data = ram_data;
    }
    n->size = size;
    n->is_dir = 0;

    /* Link into root directory's list */
    ramfs_node_t *p = root_dir;
    while (p->next) p = p->next;
    p->next = n;

    return n;
}

ramfs_node_t *ramfs_lookup(const char *path) {
    if (!path) return NULL;
    while (*path == '/') path++;
    if (!*path) return root_dir;

    ramfs_node_t *p = root_dir->next;
    while (p) {
        const char *a = path;
        const char *b = p->name;
        while (*b == '/') b++;
        int match = 1;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a != '\0' || *b != '\0') match = 0;
        while (*a == '/') a++;
        if (match && *a == '\0') return p;
        p = p->next;
    }
    return NULL;
}

void *ramfs_get_file(const char *path, uint32_t *out_size) {
    ramfs_node_t *n = ramfs_lookup(path);
    if (!n || n->is_dir) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    if (out_size) *out_size = n->size;
    return n->data;
}
