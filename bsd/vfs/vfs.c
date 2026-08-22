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


#include "bsd/vfs.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/block.h"
#include "bsd/select.h"
#include "bsd/stat.h"
#include "bsd/vfs/ext2/ext2.h"
#include "bsd/vfs/ufs/ufs.h"
#include "string.h"
#include "debug.h"
#include "vmm.h"

#define MAX_MOUNTS 8

static mount_t mounts[MAX_MOUNTS];
static int mount_count;

/* Symlink resolution budget (OpenBSD-style) */
#define VFS_SYMLINK_MAX 8

int vfs_copy_path(proc_t *p, const char *user_path, char *kpath);
static void vfs_devfs_mount(void);

/* Forward declaration (defined at the bottom of this file). */
int vfs_perm_check(proc_t *p, vnode_t *vp, int amode);

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    mount_count = 0;
    log_print(LOG_LEVEL_DEBUG, "vfs: init\r\n");
}

static mount_t *vfs_root_mount(void) {
    return mount_count > 0 ? &mounts[0] : NULL;
}

static mount_t *vfs_find_mount(const char *path) {
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].used && strcmp(mounts[i].path, path) == 0)
            return &mounts[i];
    }
    return NULL;
}

static int vfs_mount_register(block_dev_t *dev, const char *path,
                              const char *fstype) {
    if (mount_count >= MAX_MOUNTS)
        return -ENOMEM;
    if (!path || path[0] != '/')
        return -EINVAL;

    mount_t *mp = &mounts[mount_count];
    memset(mp, 0, sizeof(*mp));

    int r;
    if (!fstype || strcmp(fstype, "ufs") == 0) {
        r = ufs_mount(dev, mp);
        if (r == 0) {
            mp->type = 1;
            mp->unmount = ufs_unmount;
            mp->statfs = ufs_statfs;
            log_printf(LOG_LEVEL_INFO, "vfs: mounted %s on %s (ufs)\r\n", dev->name, path);
        }
    } else {
        r = -EINVAL;
    }
    if (r != 0) {
        r = ext2_mount(dev, mp);
        if (r == 0) {
            mp->type = 2;
            mp->unmount = ext2_unmount;
            mp->statfs = ext2_statfs;
            log_printf(LOG_LEVEL_INFO, "vfs: mounted %s on %s (ext2)\r\n", dev->name, path);
        }
    }
    if (r != 0)
        return r;

    strncpy(mp->path, path, sizeof(mp->path) - 1);
    mp->path[sizeof(mp->path) - 1] = '\0';
    mp->dev = dev;
    mp->used = 1;
    mount_count++;
    return 0;
}

/* Mount devfs at /dev when the root filesystem does not provide its own
 * "dev" directory.  This is what the old lookup-time "/dev" fallback
 * did, now expressed as a real mount. */
void vfs_devfs_mount(void) {
    mount_t *root = vfs_root_mount();
    if (!root || vfs_find_mount("/dev"))
        return;

    if (root->root && root->root->ops && root->root->ops->lookup) {
        vnode_t *devdir = root->root->ops->lookup(root->root, "dev");
        if (devdir) {
            vnode_put(devdir);
            return;   /* filesystem has its own /dev */
        }
    }

    if (mount_count >= MAX_MOUNTS)
        return;
    vnode_t *dev_root = devfs_get_root();
    if (!dev_root)
        return;
    mount_t *mp = &mounts[mount_count];
    memset(mp, 0, sizeof(*mp));
    mp->root = dev_root;
    strncpy(mp->path, "/dev", sizeof(mp->path) - 1);
    mp->path[sizeof(mp->path) - 1] = '\0';
    mp->used = 1;
    mount_count++;
    log_print(LOG_LEVEL_INFO, "vfs: devfs mounted on /dev\r\n");
}

int vfs_mount(proc_t *p, const char *udev, const char *upath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char devname[64];
    char path[128];
    if (copy_from_user(devname, udev, sizeof(devname) - 1) != 0)
        return -EFAULT;
    devname[sizeof(devname) - 1] = '\0';
    if (copy_from_user(path, upath, sizeof(path) - 1) != 0)
        return -EFAULT;
    path[sizeof(path) - 1] = '\0';

    if (!devname[0] || path[0] != '/')
        return -EINVAL;

    block_dev_t *dev = block_dev_lookup(devname);
    if (!dev)
        return -ENOENT;

    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].used && mounts[i].dev == dev)
            return -EBUSY;
        if (mounts[i].used && strcmp(mounts[i].path, path) == 0)
            return -EBUSY;
    }

    /* The mount point must exist in the parent namespace. */
    vnode_t *parent = NULL;
    char name[128];
    vfs_lookup_parent(path, &parent, name, sizeof(name));
    if (!parent) {
        vnode_t *old = vfs_lookup(path);
        if (!old)
            return -ENOENT;
        vnode_put(old);
    } else {
        vnode_put(parent);
    }

    return vfs_mount_register(dev, path, NULL);
}

int vfs_umount(proc_t *p, const char *upath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char path[128];
    if (copy_from_user(path, upath, sizeof(path) - 1) != 0)
        return -EFAULT;
    path[sizeof(path) - 1] = '\0';

    if (path[0] != '/')
        return -EINVAL;
    if (strcmp(path, "/") == 0)
        return -EBUSY;

    for (int i = 0; i < mount_count; i++) {
        mount_t *mp = &mounts[i];
        if (!mp->used || strcmp(mp->path, path) != 0)
            continue;
        if (mp->unmount) {
            int r = mp->unmount(mp);
            if (r < 0)
                return r;
        } else if (mp->root) {
            vnode_put(mp->root);
            mp->root = NULL;
        }
        mp->used = 0;
        mp->unmount = NULL;
        log_printf(LOG_LEVEL_DEBUG, "vfs: unmounted %s\r\n", path);
        return 0;
    }
    return -ENOENT;
}

/* Parse a kernel command line argument "name" from cmdline.
 * Returns 0 and fills dst if found, -1 otherwise. */
static int vfs_cmdline_arg(const char *cmdline, const char *name,
                           char *dst, size_t dst_len) {
    if (!cmdline || !name || !dst || dst_len == 0)
        return -1;
    size_t nlen = strlen(name);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t vlen = 0;
            while (v[vlen] && v[vlen] != ' ' && v[vlen] != '\t')
                vlen++;
            if (vlen >= dst_len)
                vlen = dst_len - 1;
            memcpy(dst, v, vlen);
            dst[vlen] = '\0';
            return 0;
        }
        while (*p && *p != ' ')
            p++;
    }
    return -1;
}

static int vfs_try_mount_root(block_dev_t *dev, const char *name) {
    log_printf(LOG_LEVEL_DEBUG, "vfs: trying %s\r\n", name);
    return vfs_mount_register(dev, "/", NULL);
}

int vfs_mount_root(const char *cmdline) {
    char root_dev[64];

    /* Boot-arg driven: "root=/dev/ahci0p2" (or "root=ahci0p2", "root=ramdisk"). */
    if (vfs_cmdline_arg(cmdline, "root", root_dev, sizeof(root_dev)) == 0) {
        char *name = root_dev;
        if (strncmp(name, "/dev/", 5) == 0)
            name += 5;
        log_printf(LOG_LEVEL_INFO, "vfs: root device from boot args: %s\r\n", name);
        block_dev_t *dev = block_dev_lookup(name);
        if (dev && vfs_try_mount_root(dev, name) == 0) {
            vfs_devfs_mount();
            return 0;
        }
        log_printf(LOG_LEVEL_ERROR, "vfs: cannot mount root device '%s'\r\n", name);
        /* The named device is missing (e.g. root=ahci0p2 on a PATA-only
         * machine): fall through and mount the first block device that
         * accepts a filesystem before giving up to devfs. */
    }

    /* No usable "root=" device: mount the first block device that
     * accepts a filesystem. */
    for (int i = 0; i < block_dev_get_count(); i++) {
        block_dev_t *dev = block_dev_get(i);
        if (!dev || !dev->name[0])
            continue;
        if (vfs_try_mount_root(dev, dev->name) == 0) {
            vfs_devfs_mount();
            return 0;
        }
    }

    vnode_t *root = devfs_get_root();
    if (!root) {
        log_print(LOG_LEVEL_ERROR, "vfs: failed to get devfs root\r\n");
        return -1;
    }
    mount_t *mp = &mounts[0];
    memset(mp, 0, sizeof(*mp));
    mp->root = root;
    mp->path[0] = '/';
    mp->path[1] = '\0';
    mp->used = 1;
    mount_count = 1;
    log_print(LOG_LEVEL_INFO, "vfs: root mounted (devfs)\r\n");
    return 0;
}

/* Component-wise path resolution.
 *
 * Walks the namespace from the root mount, crossing into mounted
 * filesystems when the accumulated path matches a mount point, and
 * follows symlinks (up to VFS_SYMLINK_MAX of them).  When follow_final
 * is 0 the final component is returned unresolved — vfs_readlink
 * relies on this.
 */
static vnode_t *vfs_lookup_internal(const char *path, int follow_final,
                                    int depth) {
    if (!path)
        return NULL;

    while (*path == '/')
        path++;
    if (*path == '\0') {
        mount_t *m = vfs_root_mount();
        return m ? vnode_get(m->root) : NULL;
    }

    mount_t *m = vfs_root_mount();
    if (!m || !m->root)
        return NULL;

    vnode_t *cur = vnode_get(m->root);
    if (!cur)
        return NULL;

    char acc[256];
    acc[0] = '/';
    acc[1] = '\0';
    const char *p = path;

    while (*p) {
        char component[256];
        int i = 0;
        int truncated = 0;
        while (*p && *p != '/' && i < 255)
            component[i++] = *p++;
        /* A component longer than the buffer would otherwise be looked
         * up TRUNCATED — two different paths silently resolving to the
         * same vnode is a classic confused-deputy primitive. */
        if (i == 255 && *p && *p != '/')
            truncated = 1;
        component[i] = '\0';
        while (*p == '/')
            p++;
        if (truncated) {
            vnode_put(cur);
            return NULL;
        }

        /* Accumulated path of the directory containing this component
         * (used to resolve relative symlink targets). */
        char parent_acc[256];
        strncpy(parent_acc, acc, sizeof(parent_acc) - 1);
        parent_acc[sizeof(parent_acc) - 1] = '\0';

        size_t alen = strlen(acc);
        int need_slash = (alen == 0 || acc[alen - 1] != '/');
        if (need_slash && alen + 1 + i + 1 > sizeof(acc)) {
            vnode_put(cur);
            return NULL;
        }
        if (!need_slash && alen + i + 1 > sizeof(acc)) {
            vnode_put(cur);
            return NULL;
        }
        if (need_slash) {
            acc[alen++] = '/';
            acc[alen] = '\0';
        }
        memcpy(acc + alen, component, i + 1);

        /* Cross into a mounted filesystem when the accumulated path is
         * a mount point. */
        vnode_t *next;
        mount_t *mp = vfs_find_mount(acc);
        if (mp) {
            next = mp->root ? vnode_get(mp->root) : NULL;
        } else {
            if (!cur->ops || !cur->ops->lookup) {
                vnode_put(cur);
                return NULL;
            }
            next = cur->ops->lookup(cur, component);
        }

        if (!next) {
            vnode_put(cur);
            return NULL;
        }

        /* Resolve symlinks encountered along the way (and the final
         * component too when follow_final is set). */
        if (next->type == VLNK && (follow_final || *p != '\0')) {
            if (depth >= VFS_SYMLINK_MAX || !next->ops ||
                !next->ops->readlink) {
                vnode_put(cur);
                vnode_put(next);
                return NULL;
            }
            char target[256];
            int n = next->ops->readlink(next, target, sizeof(target) - 1);
            vnode_put(next);
            vnode_put(cur);
            if (n < 0)
                return NULL;
            target[n] = '\0';

            if (target[0] == '/')
                return vfs_lookup_internal(target, follow_final, depth + 1);

            /* Relative target: resolve against the directory that
             * contained the link. */
            char combined[256];
            size_t plen = strlen(parent_acc);
            size_t tlen = strlen(target);
            if (plen + 1 + tlen + 1 > sizeof(combined))
                return NULL;
            memcpy(combined, parent_acc, plen);
            if (plen == 0 || combined[plen - 1] != '/')
                combined[plen++] = '/';
            memcpy(combined + plen, target, tlen + 1);
            return vfs_lookup_internal(combined, follow_final, depth + 1);
        }

        vnode_put(cur);
        cur = next;
        if (*p == '\0')
            break;
    }

    return cur;
}

vnode_t *vfs_lookup(const char *path) {
    return vfs_lookup_internal(path, 1, 0);
}

vnode_t *vfs_lookup_nofollow(const char *path) {
    return vfs_lookup_internal(path, 0, 0);
}


int vfs_open(proc_t *p, const char *upath, int flags, int mode) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0)
        return err;

    /* Apply the process umask to creation modes. */
    if (flags & O_CREAT)
        mode &= ~p->umask;

    /* O_NOFOLLOW: the final component must not be a symbolic link. */
    vnode_t *vp;
    if (flags & O_NOFOLLOW)
        vp = vfs_lookup_nofollow(kpath);
    else
        vp = vfs_lookup(kpath);

    if (vp && (flags & O_NOFOLLOW) && vp->type == VLNK) {
        vnode_put(vp);
        return -ELOOP;
    }

    /* O_EXCL: fail if the file already exists (only meaningful with
     * O_CREAT; without O_CREAT the flag is ignored, per POSIX). */
    if (vp && (flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_put(vp);
        return -EEXIST;
    }

    if (!vp && (flags & O_CREAT)) {
        vnode_t *parent = NULL;
        char name[256];
        vfs_lookup_parent(kpath, &parent, name, sizeof(name));
        if (parent && parent->ops && parent->ops->create) {
            if (vfs_perm_check(p, parent, W_OK) < 0) {
                vnode_put(parent);
                return -EACCES;
            }
            err = parent->ops->create(parent, name, mode, &vp);
            if (err < 0) {
                vnode_put(parent);
                return err;
            }
        }
        if (parent) vnode_put(parent);
    }
    if (!vp) return -ENOENT;

    /* O_DIRECTORY: only directories may be opened. */
    if ((flags & O_DIRECTORY) && vp->type != VDIR) {
        vnode_put(vp);
        return -ENOTDIR;
    }

    /* Permission check on the opened file itself.  Device and FIFO
     * nodes used to bypass this entirely — keep the check whenever the
     * filesystem provides a stat op (devfs nodes without stat stay
     * governed by their open path). */
    if (vp->ops && vp->ops->stat) {
        int amode = (flags & O_ACCMODE) == O_RDONLY ? R_OK
                  : (flags & O_ACCMODE) == O_WRONLY ? W_OK : (R_OK | W_OK);
        if (vfs_perm_check(p, vp, amode) < 0) {
            vnode_put(vp);
            return -EACCES;
        }
    }

    if (vp->ops && vp->ops->open) {
        err = vp->ops->open(vp, flags);
        if (err < 0) {
            vnode_put(vp);
            return err;
        }
    }

    int fd = proc_fd_alloc(p);
    if (fd < 0) {
        vnode_put(vp);
        return -EMFILE;
    }
    p->fds[fd].vnode_ptr = (void *)vp;
    p->fds[fd].flags     = flags;
    p->fds[fd].offset    = 0;
    p->fds[fd].mode      = 0;
    p->fds[fd].cloexec   = (flags & O_CLOEXEC) ? 1 : 0;
    p->fds[fd].used      = 1;

    return fd;
}

int vfs_close(proc_t *p, int fd) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (vp) vnode_put(vp);
    proc_fd_dealloc(p, fd);
    return 0;
}

ssize_t vfs_read(proc_t *p, int fd, void *buf, size_t count) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->read) return -EINVAL;

    /* O_NONBLOCK on a pipe or tty: never block — if the descriptor is
     * not readable right now, fail with -EAGAIN (POSIX). */
    if ((f->flags & O_NONBLOCK) && (vp->type == VFIFO || vp->type == VCHR) &&
        vp->ops->poll) {
        if (!(vp->ops->poll(vp, POLLIN | POLLRDNORM) & (POLLIN | POLLRDNORM)))
            return -EAGAIN;
    }

    /* Buffer validation BEFORE any vnode op touches `buf`: it is a
     * raw user pointer.  A kernel address here would hand the file's
     * contents an arbitrary kernel write; an unmapped one would fault
     * in ring 0 and panic. */
    if (!user_range_ok(buf, count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)count, 1))
        return -EFAULT;

    if (vp->ops->stat) {
        if (vfs_perm_check(p, vp, R_OK) < 0)
            return -EACCES;
    }

    ssize_t ret = vp->ops->read(vp, buf, count, f->offset);
    if (ret > 0) f->offset += ret;
    return ret;
}

/* pread(2): read at an explicit offset without changing the
 * descriptor's file offset.  POSIX: invalid on pipes and devices. */
ssize_t vfs_pread(proc_t *p, int fd, void *buf, size_t count, int64_t offset) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->read) return -EINVAL;

    if (vp->type == VFIFO || vp->type == VCHR)
        return -ESPIPE;
    if (offset < 0)
        return -EINVAL;

    if (!user_range_ok(buf, count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)count, 1))
        return -EFAULT;

    if (vp->ops->stat) {
        if (vfs_perm_check(p, vp, R_OK) < 0)
            return -EACCES;
    }
    return vp->ops->read(vp, buf, count, offset);
}

ssize_t vfs_write(proc_t *p, int fd, const void *buf, size_t count) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->write) return -EINVAL;

    /* O_NONBLOCK on a pipe or tty: if the descriptor cannot take data
     * right now, fail with -EAGAIN instead of blocking. */
    if ((f->flags & O_NONBLOCK) && (vp->type == VFIFO || vp->type == VCHR) &&
        vp->ops->poll) {
        if (!(vp->ops->poll(vp, POLLOUT | POLLWRNORM) & (POLLOUT | POLLWRNORM)))
            return -EAGAIN;
    }

    if (vp->ops->stat) {
        if (vfs_perm_check(p, vp, W_OK) < 0)
            return -EACCES;
    }

    /* O_APPEND: every write goes to the current end of the file.
     * All fds on the same vnode share vp->size, so concurrent appends
     * to the same file each land after the previous one. */
    if (!user_range_ok(buf, count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)count, 0))
        return -EFAULT;

    int64_t off = (f->flags & O_APPEND) ? vp->size : f->offset;
    ssize_t ret = vp->ops->write(vp, buf, count, off);
    if (ret > 0) {
        if (f->flags & O_APPEND)
            f->offset = vp->size;
        else
            f->offset += ret;
    }
    return ret;
}

/* pwrite(2): write at an explicit offset without changing the
 * descriptor's file offset.  POSIX: invalid on pipes and devices. */
ssize_t vfs_pwrite(proc_t *p, int fd, const void *buf, size_t count, int64_t offset) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->write) return -EINVAL;

    if (vp->type == VFIFO || vp->type == VCHR)
        return -ESPIPE;
    if (offset < 0)
        return -EINVAL;

    if (!user_range_ok(buf, count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)count, 0))
        return -EFAULT;

    if (vp->ops->stat) {
        if (vfs_perm_check(p, vp, W_OK) < 0)
            return -EACCES;
    }
    return vp->ops->write(vp, buf, count, offset);
}

int vfs_ioctl(proc_t *p, int fd, int cmd, void *data) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->ioctl) return -ENOTTY;

    return vp->ops->ioctl(vp, cmd, data);
}

int64_t vfs_lseek(proc_t *p, int fd, int64_t offset, int whence) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    int64_t new_off;
    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = f->offset + offset; break;
    case SEEK_END: new_off = vp->size + offset; break;
    default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    f->offset = new_off;
    return new_off;
}


/* Split a path into its parent directory and final component.
 * On success returns the parent vnode (caller must vnode_put). */
vnode_t *vfs_lookup_parent(const char *path, vnode_t **parent,
                           char *name, size_t name_len) {
    *parent = NULL;
    if (name && name_len) name[0] = '\0';
    if (!path || !*path) return NULL;

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return NULL;

    const char *slash = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/') slash = q;
    }

    if (slash == p) {
        /* "name" only: parent is the root */
        vnode_t *root = vfs_lookup("/");
        *parent = root;
        if (name && name_len) {
            size_t n = strlen(p);
            if (n >= name_len) n = name_len - 1;
            memcpy(name, p, n);
            name[n] = '\0';
        }
        return root;
    }

    /* parent path is [p, slash) */
    char parent_path[256];
    size_t plen = (size_t)(slash - p);
    if (plen >= sizeof(parent_path)) plen = sizeof(parent_path) - 1;
    memcpy(parent_path, p, plen);
    parent_path[plen] = '\0';

    vnode_t *dir = vfs_lookup(parent_path);
    *parent = dir;
    if (name && name_len) {
        size_t n = strlen(slash + 1);
        if (n >= name_len) n = name_len - 1;
        memcpy(name, slash + 1, n);
        name[n] = '\0';
    }
    return dir;
}

/* Build an absolute, normalized path for `in` relative to `cwd`:
 * prepend cwd to relative paths, then lexically collapse '/' runs and
 * resolve '.' and '..'.  `cwd` is assumed absolute and normalized.
 * Returns 0, or -ENAMETOOLONG if the result does not fit. */
int vfs_build_abs_path(const char *cwd, const char *in, char *out, size_t outsz) {
    if (!in || !out || outsz < 2)
        return -ENAMETOOLONG;

    size_t n = 0;
    const char *src = in;

    /* Base: root for absolute input, otherwise the current directory. */
    if (*src == '/') {
        out[n++] = '/';
        while (*src == '/')
            src++;
    } else {
        const char *c = (cwd && cwd[0] == '/') ? cwd : "/";
        for (; *c; c++) {
            if (n + 2 >= outsz)
                return -ENAMETOOLONG;
            out[n++] = *c;
        }
        if (n == 0) {
            if (n + 1 >= outsz)
                return -ENAMETOOLONG;
            out[n++] = '/';
        }
    }

    while (*src) {
        const char *t = src;
        while (*src && *src != '/')
            src++;
        size_t tlen = (size_t)(src - t);
        while (*src == '/')
            src++;

        if (tlen == 0)
            continue;
        if (tlen == 1 && t[0] == '.')
            continue;
        if (tlen == 2 && t[0] == '.' && t[1] == '.') {
            /* Pop the last component (staying at the root). */
            if (n > 1) {
                n--;
                while (n > 0 && out[n - 1] != '/')
                    n--;
                if (n < 1)
                    n = 1;
            }
            continue;
        }
        if (out[n - 1] != '/') {
            if (n + 2 >= outsz)
                return -ENAMETOOLONG;
            out[n++] = '/';
        }
        if (n + tlen + 1 > outsz)
            return -ENAMETOOLONG;
        memcpy(out + n, t, tlen);
        n += tlen;
    }
    out[n] = '\0';
    return 0;
}

/* Copy a user path into a kernel buffer, reject paths that do not fit,
 * and resolve relative paths against the process's current directory.
 * The result is absolute and normalized. */
int vfs_copy_path(proc_t *p, const char *user_path, char *kpath) {
    char tmp[256];
    if (copy_from_user(tmp, user_path, sizeof(tmp) - 1) != 0)
        return -EFAULT;
    tmp[sizeof(tmp) - 1] = '\0';

    /* The string must end with a NUL inside the 255-byte window. */
    int found = 0;
    for (int i = 0; i < 255; i++)
        if (tmp[i] == '\0') { found = 1; break; }
    if (!found) {
        /* No NUL in the window: only an exactly-255-char path (NUL at
         * byte 255) is still acceptable; anything longer is too long. */
        uint8_t b;
        if (copy_from_user(&b, user_path + 255, 1) == 0 && b != '\0')
            return -ENAMETOOLONG;
    }

    return vfs_build_abs_path(p ? p->cwd : "/", tmp, kpath, 256);
}

int vfs_mkdir(proc_t *p, const char *upath, int mode) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    mode &= ~p->umask;

    vnode_t *parent = NULL;
    char name[256];
    if (!vfs_lookup_parent(kpath, &parent, name, sizeof(name)) || !parent)
        return -ENOENT;
    if (vfs_perm_check(p, parent, W_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        vnode_put(parent);
        return -EINVAL;
    }
    err = parent->ops->mkdir(parent, name, mode);
    vnode_put(parent);
    return err;
}

int vfs_rmdir(proc_t *p, const char *upath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *parent = NULL;
    char name[256];
    if (!vfs_lookup_parent(kpath, &parent, name, sizeof(name)) || !parent)
        return -ENOENT;
    if (vfs_perm_check(p, parent, W_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->rmdir) {
        vnode_put(parent);
        return -EINVAL;
    }
    err = parent->ops->rmdir(parent, name);
    vnode_put(parent);
    return err;
}

int vfs_unlink(proc_t *p, const char *upath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *parent = NULL;
    char name[256];
    if (!vfs_lookup_parent(kpath, &parent, name, sizeof(name)) || !parent)
        return -ENOENT;
    if (vfs_perm_check(p, parent, W_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->unlink) {
        vnode_put(parent);
        return -EINVAL;
    }
    err = parent->ops->unlink(parent, name);
    vnode_put(parent);
    return err;
}

int vfs_symlink(proc_t *p, const char *utarget, const char *ulinkpath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char ktarget[256];
    if (vfs_copy_path(p, utarget, ktarget) < 0)
        return -EFAULT;
    char kpath[256];
    int err = vfs_copy_path(p, ulinkpath, kpath);
    if (err < 0) return err;

    vnode_t *parent = NULL;
    char name[256];
    if (!vfs_lookup_parent(kpath, &parent, name, sizeof(name)) || !parent)
        return -ENOENT;
    if (vfs_perm_check(p, parent, W_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->symlink) {
        vnode_put(parent);
        return -EINVAL;
    }
    err = parent->ops->symlink(parent, name, ktarget);
    vnode_put(parent);
    return err;
}

int vfs_link(proc_t *p, const char *uoldpath, const char *unewpath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kold[256];
    int err = vfs_copy_path(p, uoldpath, kold);
    if (err < 0) return err;
    char knew[256];
    err = vfs_copy_path(p, unewpath, knew);
    if (err < 0) return err;

    vnode_t *target = vfs_lookup(kold);
    if (!target) return -ENOENT;

    vnode_t *parent = NULL;
    char name[256];
    if (!vfs_lookup_parent(knew, &parent, name, sizeof(name)) || !parent) {
        vnode_put(target);
        return -ENOENT;
    }
    if (vfs_perm_check(p, parent, W_OK) < 0) {
        vnode_put(target);
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->link) {
        vnode_put(target);
        vnode_put(parent);
        return -EINVAL;
    }
    err = parent->ops->link(parent, name, target);
    vnode_put(target);
    vnode_put(parent);
    return err;
}

int vfs_readlink(proc_t *p, const char *upath, char *buf, size_t buflen) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup_nofollow(kpath);
    if (!vp) return -ENOENT;
    if (!vp->ops || !vp->ops->readlink) {
        vnode_put(vp);
        return -EINVAL;
    }
    err = vp->ops->readlink(vp, buf, buflen);
    vnode_put(vp);
    return err;
}

int vfs_rename(proc_t *p, const char *uoldpath, const char *unewpath) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kold[256];
    int err = vfs_copy_path(p, uoldpath, kold);
    if (err < 0) return err;
    char knew[256];
    err = vfs_copy_path(p, unewpath, knew);
    if (err < 0) return err;

    vnode_t *src_dir = NULL, *dst_dir = NULL;
    char src_name[256], dst_name[256];
    if (!vfs_lookup_parent(kold, &src_dir, src_name, sizeof(src_name)) || !src_dir)
        return -ENOENT;
    if (!vfs_lookup_parent(knew, &dst_dir, dst_name, sizeof(dst_name)) || !dst_dir) {
        vnode_put(src_dir);
        return -ENOENT;
    }

    if (vfs_perm_check(p, src_dir, W_OK) < 0 ||
        vfs_perm_check(p, dst_dir, W_OK) < 0) {
        vnode_put(src_dir);
        vnode_put(dst_dir);
        return -EACCES;
    }

    if (!src_dir->ops || !src_dir->ops->rename) {
        vnode_put(src_dir);
        vnode_put(dst_dir);
        return -EINVAL;
    }
    err = src_dir->ops->rename(src_dir, src_name, dst_dir, dst_name);
    vnode_put(src_dir);
    vnode_put(dst_dir);
    return err;
}

int vfs_stat(proc_t *p, const char *upath, void *statbuf) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup(kpath);
    if (!vp) return -ENOENT;
    if (!vp->ops || !vp->ops->stat) {
        vnode_put(vp);
        return -ENOSYS;
    }
    err = vp->ops->stat(vp, statbuf);
    vnode_put(vp);
    return err;
}

/* lstat(2): stat without following a final symlink. */
int vfs_lstat(proc_t *p, const char *upath, void *statbuf) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup_nofollow(kpath);
    if (!vp) return -ENOENT;
    if (!vp->ops || !vp->ops->stat) {
        vnode_put(vp);
        return -ENOSYS;
    }
    err = vp->ops->stat(vp, statbuf);
    vnode_put(vp);
    return err;
}

int vfs_fstat(proc_t *p, int fd, void *statbuf) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->stat) return -ENOSYS;
    return vp->ops->stat(vp, statbuf);
}


/* Check `amode` (R_OK/W_OK/X_OK) against the vnode's mode bits using
 * the process credentials (euid/egid).  Root can do anything except
 * execute files with no execute bit at all. */
int vfs_perm_check(proc_t *p, vnode_t *vp, int amode) {
    if (!p || !vp)
        return -EACCES;
    if (!vp->ops || !vp->ops->stat)
        return 0;   /* devices/pipes: no permission model */

    struct stat st;
    if (vp->ops->stat(vp, &st) < 0)
        return -EACCES;

    int perm = 0;
    if (p->euid == st.st_uid) {
        perm = (st.st_mode >> 6) & 7;
    } else if (p->egid == st.st_gid) {
        perm = (st.st_mode >> 3) & 7;
    } else {
        perm = st.st_mode & 7;
    }

    if (amode & X_OK) {
        if (p->euid == 0 && (st.st_mode & 0111) == 0)
            return -EACCES;
    }
    if (amode & R_OK && !(perm & 4))
        return -EACCES;
    if (amode & W_OK && !(perm & 2))
        return -EACCES;
    if (amode & X_OK && !(perm & 1))
        return -EACCES;
    return 0;
}

int vfs_access(proc_t *p, const char *upath, int mode) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup(kpath);
    if (!vp) return -ENOENT;
    if (mode == F_OK) {
        vnode_put(vp);
        return 0;
    }
    err = vfs_perm_check(p, vp, mode);
    vnode_put(vp);
    return err;
}

int vfs_chmod(proc_t *p, const char *upath, int mode) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup(kpath);
    if (!vp) return -ENOENT;
    if (!vp->ops || !vp->ops->chmod) {
        vnode_put(vp);
        return -EINVAL;
    }
    err = vp->ops->chmod(vp, mode & 07777);
    vnode_put(vp);
    return err;
}

int vfs_chown(proc_t *p, const char *upath, int uid, int gid) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    /* Only root may change ownership. */
    if (p->euid != 0)
        return -EPERM;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup(kpath);
    if (!vp) return -ENOENT;
    if (!vp->ops || !vp->ops->chown) {
        vnode_put(vp);
        return -EINVAL;
    }
    err = vp->ops->chown(vp, uid, gid);
    vnode_put(vp);
    return err;
}

int vfs_truncate(proc_t *p, const char *upath, int64_t length) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;
    if (length < 0) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    vnode_t *vp = vfs_lookup(kpath);
    if (!vp) return -ENOENT;
    if (vp->type == VDIR) {
        vnode_put(vp);
        return -EISDIR;
    }
    if (vfs_perm_check(p, vp, W_OK) < 0) {
        vnode_put(vp);
        return -EACCES;
    }
    if (!vp->ops || !vp->ops->truncate) {
        vnode_put(vp);
        return -EINVAL;
    }
    err = vp->ops->truncate(vp, length);
    if (err == 0)
        vp->size = length;
    vnode_put(vp);
    return err;
}

int vfs_ftruncate(proc_t *p, int fd, int64_t length) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;
    if (length < 0) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp) return -EINVAL;
    if (vp->type == VDIR) return -EISDIR;
    if (vp->ops->stat) {
        if (vfs_perm_check(p, vp, W_OK) < 0)
            return -EACCES;
    }
    if (!vp->ops || !vp->ops->truncate) return -EINVAL;
    int err = vp->ops->truncate(vp, length);
    if (err == 0)
        vp->size = length;
    return err;
}

int vfs_fsync(proc_t *p, int fd) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp) return -EINVAL;
    if (!vp->ops || !vp->ops->fsync) return 0;   /* nothing to flush */
    return vp->ops->fsync(vp);
}

int vfs_getdents(proc_t *p, int fd, void *buf, size_t count) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp) return -EINVAL;
    if (vp->type != VDIR) return -ENOTDIR;
    if (!vp->ops || !vp->ops->getdents) return -EINVAL;

    int64_t off = f->offset;
    int ret = vp->ops->getdents(vp, buf, count, &off);
    if (ret > 0)
        f->offset = off;
    return ret;
}

int vfs_statvfs(proc_t *p, const char *upath, void *stbuf) {
    if (!p) p = proc_current();
    if (!p) return -EINVAL;

    char kpath[256];
    int err = vfs_copy_path(p, upath, kpath);
    if (err < 0) return err;

    /* Find the mount containing the path (only root mounts matter). */
    mount_t *mp = NULL;
    if (strcmp(kpath, "/") == 0 || kpath[0] != '/') {
        mp = vfs_root_mount();
    } else {
        char acc[256];
        acc[0] = '/';
        acc[1] = '\0';
        const char *q = kpath + 1;
        while (*q) {
            while (*q == '/')
                q++;
            if (!*q)
                break;
            char comp[128];
            int i = 0;
            while (*q && *q != '/' && i < 127)
                comp[i++] = *q++;
            comp[i] = '\0';
            size_t alen = strlen(acc);
            if (alen > 1 && acc[alen - 1] != '/')
                acc[alen++] = '/';
            memcpy(acc + alen, comp, i + 1);
            mount_t *m = vfs_find_mount(acc);
            if (m) {
                mp = m;
                break;
            }
        }
        if (!mp)
            mp = vfs_root_mount();
    }

    if (!mp) return -ENOENT;
    if (!mp->statfs) return -ENOSYS;
    return mp->statfs(mp, stbuf);
}

int vfs_fd_poll(proc_t *p, int fd, int events) {
    if (!p) p = proc_current();
    if (!p) return POLLNVAL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return POLLNVAL;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp) return POLLNVAL;
    if (vp->ops && vp->ops->poll)
        return vp->ops->poll(vp, events);
    if (vp->type == VDIR || vp->type == VREG)
        return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
    return POLLIN | POLLOUT;
}

waitq_t *vfs_fd_poll_waitq(proc_t *p, int fd) {
    if (!p) p = proc_current();
    if (!p) return NULL;

    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return NULL;

    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || !vp->ops || !vp->ops->poll_waitq)
        return NULL;
    return vp->ops->poll_waitq(vp);
}

int vfs_dup2(proc_t *p, int oldfd, int newfd) {
    if (!p) p = proc_current();
    if (!p) return -EBADF;
    if (newfd < 0 || newfd >= FD_MAX)
        return -EBADF;
    return proc_fd_dup2(p, p, oldfd, newfd);
}
