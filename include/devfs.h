#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include "fd.h"

int  devfs_is_dev_path(const char *path);
int  devfs_readdir(const char *path, char *buf, uint32_t size, uint32_t *out_bytes);
int  devfs_getdents(const char *path, dirent_t *dirp, uint32_t count);
int  devfs_open(const char *path, uint32_t *out_type, void **out_priv);

#endif
