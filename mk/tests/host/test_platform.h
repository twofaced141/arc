/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H

/*
 * Host-side platform shim for the ext2 driver.  Compiled only when
 * HOST_TEST_EXT2 is defined (see the arc Makefile).  Lets ext2.c run
 * verbatim as a userspace program so it can be verified against
 * real e2fsprogs tools.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>  /* ssize_t */
#include <string.h>   /* memcpy/memset/strcmp/strncmp/strlen */
#include <stdlib.h>   /* malloc/free */
#include <stdio.h>

#include "bsd/block.h"

#define kmalloc(x) malloc(x)
#define kfree(x)   free(x)

#define blk_read(dev, lba, buf, count)  ((dev)->read((dev), (lba), (buf), (count)))
#define blk_write(dev, lba, buf, count) ((dev)->write((dev), (lba), (buf), (count)))

/* The host has no dirty block cache, so syncing is a no-op. */
#define blk_sync(dev) ((void)(dev), 0)

#define debug_print(s)  ((void)0)
#define debug_printf(...) ((void)0)

#define log_print(level, s)      ((void)0)
#define log_printf(level, ...)   ((void)0)
#define log_get_level()          (0)

static inline long ext2_now_sec(void) {
    return 1700000000L;
}

static inline long ufs_now_sec(void) {
    return 1700000000L;
}

/* ---- open flags (subset of POSIX, mirrors bsd/vfs.h) ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT    0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_CLOEXEC 0x8000

#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

/* ---- vnode types (mirrors bsd/vfs.h) ---- */
#define VREG     1
#define VDIR     2
#define VLNK     5

/* ---- vnode structures (mirror bsd/vfs.h, minus kernel-only fields) ---- */
struct vnode_ops;
struct mount;

typedef struct vnode {
    int    type;
    int    flags;
    int    refcount;
    int    ino;
    int    minor;
    struct mount *mount;
    int64_t size;
    struct vnode_ops *ops;
    void  *data;
    char   name[64];
    struct vnode *vnext;
    struct vnode *lru_prev;
    struct vnode *lru_next;
    uint8_t in_hash;
    uint8_t cached;
} vnode_t;

typedef struct mount {
    vnode_t *root;
    int      type;
    void    *data;
    struct block_dev *dev;
    char     path[128];
    int      used;
    int      (*unmount)(struct mount *mp);
} mount_t;

struct vnode_ops {
    int (*open)(struct vnode *vp, int mode);
    int (*close)(struct vnode *vp);
    ssize_t (*read)(struct vnode *vp, void *buf, size_t count, int64_t offset);
    ssize_t (*write)(struct vnode *vp, const void *buf, size_t count, int64_t offset);
    int (*lseek)(struct vnode *vp, int64_t offset, int whence);
    int (*stat)(struct vnode *vp, void *statbuf);
    int (*ioctl)(struct vnode *vp, int cmd, void *data);
    int (*mmap)(struct vnode *vp, uint32_t addr, size_t len, int prot, int flags);
    struct vnode *(*lookup)(struct vnode *vp, const char *name);
    int (*create)(struct vnode *dir, const char *name, int mode,
                  struct vnode **out);
    int (*mkdir)(struct vnode *dir, const char *name, int mode);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*rmdir)(struct vnode *dir, const char *name);
    int (*link)(struct vnode *dir, const char *name, struct vnode *target);
    int (*symlink)(struct vnode *dir, const char *name, const char *target);
    int (*readlink)(struct vnode *vp, char *buf, size_t buflen);
    int (*rename)(struct vnode *src_dir, const char *src,
                  struct vnode *dst_dir, const char *dst);
    int (*chmod)(struct vnode *vp, int mode);
    int (*chown)(struct vnode *vp, int uid, int gid);
    int (*truncate)(struct vnode *vp, int64_t length);
    int (*fsync)(struct vnode *vp);
    int (*getdents)(struct vnode *vp, void *buf, size_t count,
                    int64_t *offset);
    int (*poll)(struct vnode *vp, int events);
};
typedef struct vnode_ops vnode_ops_t;

/* vnode_alloc lives in the kernel's vnode.c; provide it here. */
static inline vnode_t *vnode_alloc(void) {
    vnode_t *vp = (vnode_t *)calloc(1, sizeof(vnode_t));
    if (vp) vp->refcount = 1;
    return vp;
}

/* Host-side stand-ins for the kernel's vnode cache (vnode.c).  There is
 * no cache on the host: every get is a fresh, unreferenced vnode. */
static inline vnode_t *vnode_cache_get(struct mount *m, int ino) {
    (void)m;
    vnode_t *vp = vnode_alloc();
    if (vp) vp->ino = ino;
    return vp;
}

static inline int vnode_cache_commit(vnode_t *vp) {
    (void)vp;
    return 0;
}

static inline void vnode_cache_invalidate(struct mount *m, int ino) {
    (void)m; (void)ino;
}

static inline void vnode_cache_flush_mount(struct mount *m) {
    (void)m;
}

static inline void vnode_put(vnode_t *vp) {
    if (vp && --vp->refcount <= 0)
        free(vp);
}

#endif