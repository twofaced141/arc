#include "devfs.h"
#include "fd.h"
#include "block.h"
#include <string.h>
#include <stddef.h>

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int strstart(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

int devfs_is_dev_path(const char *path) {
    if (!path) return 0;
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' &&
        (path[4] == '\0' || path[4] == '/'))
        return 1;
    return 0;
}

/* Map a path like /dev/sda0 to a block device */
static block_device_t *dev_to_block(const char *path) {
    if (!path || !strstart(path, "/dev/"))
        return NULL;
    const char *name = path + 5;

    for (int i = 0; i < block_device_count(); i++) {
        block_device_t *dev = block_device_get(i);
        if (streq(name, dev->name))
            return dev;
    }
    return NULL;
}

int devfs_open(const char *path, uint32_t *out_type, void **out_priv) {
    if (streq(path, "/dev/null")) {
        *out_type = FD_NULL;
        *out_priv = NULL;
        return 0;
    }
    if (streq(path, "/dev/zero")) {
        *out_type = FD_ZERO;
        *out_priv = NULL;
        return 0;
    }
    if (streq(path, "/dev/random") || streq(path, "/dev/urandom")) {
        *out_type = FD_RANDOM;
        *out_priv = NULL;
        return 0;
    }

    block_device_t *blk = dev_to_block(path);
    if (blk) {
        *out_type = FD_BLK;
        *out_priv = (void *)blk;
        return 0;
    }

    return -1;
}

int devfs_readdir(const char *path, char *buf, uint32_t size, uint32_t *out_bytes) {
    uint32_t bytes = 0;

    if (streq(path, "/dev") || streq(path, "/dev/")) {
        const char *entries[] = {"null", "zero", "random", "urandom", NULL};
        for (int i = 0; entries[i]; i++) {
            const char *s = entries[i];
            while (*s && bytes < size) buf[bytes++] = *s++;
            if (bytes < size) buf[bytes++] = '\n';
        }
        /* Add block devices */
        for (int i = 0; i < block_device_count(); i++) {
            block_device_t *dev = block_device_get(i);
            const char *s = dev->name;
            while (*s && bytes < size) buf[bytes++] = *s++;
            if (bytes < size) buf[bytes++] = '\n';
        }
        *out_bytes = bytes;
        return 0;
    }

    return -1;
}

int devfs_getdents(const char *path, dirent_t *dirp, uint32_t count) {
    uint32_t written = 0;

    if (streq(path, "/dev") || streq(path, "/dev/")) {
        struct { const char *name; uint8_t type; } entries[] = {
            {"null", 1}, {"zero", 1}, {"random", 1}, {"urandom", 1}, {NULL, 0}
        };
        for (int i = 0; entries[i].name && written < count; i++) {
            dirent_t *d = &dirp[written];
            d->d_ino = 2;
            d->d_off = written;
            d->d_type = entries[i].type;
            d->d_reclen = sizeof(dirent_t);
            int j = 0;
            while (entries[i].name[j] && j < 255) { d->d_name[j] = entries[i].name[j]; j++; }
            d->d_name[j] = '\0';
            written++;
        }
        /* Add block devices */
        for (int i = 0; i < block_device_count() && written < count; i++) {
            block_device_t *dev = block_device_get(i);
            dirent_t *d = &dirp[written];
            d->d_ino = 3 + i;
            d->d_off = written;
            d->d_type = 6;
            d->d_reclen = sizeof(dirent_t);
            int j = 0;
            while (dev->name[j] && j < 255) { d->d_name[j] = dev->name[j]; j++; }
            d->d_name[j] = '\0';
            written++;
        }
        return (int)written;
    }

    return -1;
}
