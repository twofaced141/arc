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


#ifndef BSD_VFS_H
#define BSD_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"
#include "proc.h"
#include "stat.h"

#ifndef ssize_t
typedef long ssize_t;
#endif

/* Vnode types */
#define VNON     0
#define VREG     1   /* regular file */
#define VDIR     2   /* directory */
#define VBLK     3   /* block device */
#define VCHR     4   /* character device */
#define VLNK     5   /* symbolic link */
#define VSOCK    6   /* socket */
#define VNEG     7   /* negative cache entry */
#define VFIFO    8   /* pipe */

/* Vnode flags */
#define VROOT    (1 << 0)
#define VDOOMED  (1 << 1)

/* Open flags (Linux-compatible values) */
#define O_ACCMODE    3
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT    0x40
#define O_EXCL     0x80
#define O_NOCTTY  0x100
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_NONBLOCK 0x800
#define O_CLOEXEC 0x8000
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000

/* fcntl(2) commands (POSIX) */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

/* Lseek whence */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

/* Inode numbers for devfs */
#define DEVFS_ROOT   1
#define DEVFS_NULL   2
#define DEVFS_ZERO   3
#define DEVFS_CONSOLE 4
/* Dynamic block-device nodes start here */
#define DEVFS_BLOCK_BASE 100

/* File mode bits */
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

/* Vnode operations table */
struct vnode;
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
    /* waitq to block on while poll(ing) this vnode: activity on the
     * descriptor wakes waiters early (NULL: sleep only on the proc
     * waitq, i.e. signal/deadline wakeups). */
    waitq_t *(*poll_waitq)(struct vnode *vp);
};

/* Vnode structure */
typedef struct vnode {
    int    type;        /* VREG, VDIR, VCHR, etc. */
    int    flags;
    int    refcount;
    
    /* Inode / dev identifier */
    int    ino;
    
    /* For character devices: minor number */
    int    minor;
    
    /* Mount point */
    struct mount *mount;
    
    /* Size */
    int64_t size;
    
    /* Operations */
    struct vnode_ops *ops;
    
    /* Private data (filesystem-specific) */
    void   *data;
    
    /* Name (for devfs) */
    char   name[64];
    
    spinlock_t lock;

    /* Vnode cache links (vnode.c) */
    struct vnode *vnext;
    struct vnode *lru_prev;
    struct vnode *lru_next;
    uint8_t in_hash;
    uint8_t cached;
} vnode_t;

/* Mount structure */
struct block_dev;

typedef struct mount {
    vnode_t *root;
    int      type;     /* filesystem type */
    void    *data;
    struct block_dev *dev;
    char     path[128];
    int      used;
    int      (*unmount)(struct mount *mp);
    int      (*statfs)(struct mount *mp, void *stbuf);
} mount_t;

/* File descriptor reference from proc */
/* (declared in proc.h as filedesc_t) */

/* VFS API */
void    vfs_init(void);
int     vfs_mount_root(const char *cmdline);
int     vfs_mount(proc_t *p, const char *devpath, const char *path);
int     vfs_umount(proc_t *p, const char *path);

/* Path resolution */
vnode_t *vfs_lookup(const char *path);
vnode_t *vfs_lookup_nofollow(const char *path);
int     vfs_build_abs_path(const char *cwd, const char *in, char *out, size_t outsz);
int     vfs_copy_path(proc_t *p, const char *user_path, char *kpath);
int     vfs_perm_check(proc_t *p, vnode_t *vp, int amode);

/* VFS-level file operations (called from syscall dispatcher helpers) */
int     vfs_open(proc_t *p, const char *path, int flags, int mode);
int     vfs_close(proc_t *p, int fd);
ssize_t vfs_read(proc_t *p, int fd, void *buf, size_t count);
ssize_t vfs_write(proc_t *p, int fd, const void *buf, size_t count);
int     vfs_ioctl(proc_t *p, int fd, int cmd, void *data);
int64_t vfs_lseek(proc_t *p, int fd, int64_t offset, int whence);

/* Namespace-changing operations */
vnode_t *vfs_lookup_parent(const char *path, vnode_t **parent, char *name, size_t name_len);
int     vfs_mkdir(proc_t *p, const char *path, int mode);
int     vfs_rmdir(proc_t *p, const char *path);
int     vfs_unlink(proc_t *p, const char *path);
int     vfs_symlink(proc_t *p, const char *target, const char *linkpath);
int     vfs_link(proc_t *p, const char *oldpath, const char *newpath);
int     vfs_readlink(proc_t *p, const char *path, char *buf, size_t buflen);
int     vfs_rename(proc_t *p, const char *oldpath, const char *newpath);

/* Stat */
int     vfs_stat(proc_t *p, const char *path, void *statbuf);
int     vfs_lstat(proc_t *p, const char *path, void *statbuf);
int     vfs_fstat(proc_t *p, int fd, void *statbuf);

/* Positional I/O (pread/pwrite): explicit offset, never moves the
 * descriptor's file offset. */
ssize_t vfs_pread(proc_t *p, int fd, void *buf, size_t count, int64_t offset);
ssize_t vfs_pwrite(proc_t *p, int fd, const void *buf, size_t count, int64_t offset);

/* File attributes and access checks */
int     vfs_access(proc_t *p, const char *path, int mode);
int     vfs_chmod(proc_t *p, const char *path, int mode);
int     vfs_chown(proc_t *p, const char *path, int uid, int gid);
int     vfs_truncate(proc_t *p, const char *path, int64_t length);
int     vfs_ftruncate(proc_t *p, int fd, int64_t length);
int     vfs_fsync(proc_t *p, int fd);

/* Directory listing */
int     vfs_getdents(proc_t *p, int fd, void *buf, size_t count);

/* Filesystem status */
int     vfs_statvfs(proc_t *p, const char *path, void *stbuf);

/* Readiness (select/poll) — returns revents mask */
int     vfs_fd_poll(proc_t *p, int fd, int events);

/* Wait queue to block on while waiting for fd readiness (NULL if none) */
waitq_t *vfs_fd_poll_waitq(proc_t *p, int fd);

/* FD duplication at an explicit target slot (dup2) */
int     vfs_dup2(proc_t *p, int oldfd, int newfd);

/* Pipes (bsd/uipc/pipe.c) */
int     pipe_create(proc_t *p, int fds[2]);

/* Devfs */
void    devfs_init(void);
vnode_t *devfs_get_root(void);

/* Vnode refcounting (from vnode.c) */
void    vnode_ref(vnode_t *vp);
void    vnode_unref(vnode_t *vp);
vnode_t *vnode_get(vnode_t *vp);
void    vnode_put(vnode_t *vp);
vnode_t *vnode_alloc(void);

/* Vnode cache (from vnode.c).
 * vnode_cache_get returns a vnode with a reference held.  On a miss the
 * returned vnode is freshly allocated and NOT yet visible to other
 * lookups: the caller must fill vp->data (and the fs-specific fields)
 * and then call vnode_cache_commit.  A cache hit is a vnode that was
 * already committed, so vp->data is already valid. */
vnode_t *vnode_cache_get(struct mount *m, int ino);
int      vnode_cache_commit(vnode_t *vp);
void     vnode_cache_invalidate(struct mount *m, int ino);
void     vnode_cache_flush_mount(struct mount *m);

#endif
