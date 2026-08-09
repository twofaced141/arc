#include "bsd/vfs.h"
#include "bsd/errno.h"
#include "bsd/dirent.h"
#include "bsd/select.h"
#include "bsd/tty.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"
#include "vmm.h"

static int null_open(vnode_t *vp, int mode) { (void)vp; (void)mode; return 0; }
static int null_close(vnode_t *vp)          { (void)vp; return 0; }
static ssize_t null_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    (void)vp; (void)buf; (void)count; (void)offset; return 0;
}
static ssize_t null_write(vnode_t *vp, const void *buf, size_t count, int64_t offset) {
    (void)vp; (void)buf; (void)offset; return (ssize_t)count;
}
static int null_lseek(vnode_t *vp, int64_t offset, int whence) {
    (void)vp; (void)offset; (void)whence; return 0;
}
static int null_poll(vnode_t *vp, int events) {
    (void)vp;
    return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

static struct vnode_ops dev_null_ops = {
    .open  = null_open,
    .close = null_close,
    .read  = null_read,
    .write = null_write,
    .lseek = null_lseek,
    .poll  = null_poll,
};

static int zero_open(vnode_t *vp, int mode) { (void)vp; (void)mode; return 0; }
static int zero_close(vnode_t *vp)          { (void)vp; return 0; }
static ssize_t zero_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    (void)vp; (void)offset;
    memset(buf, 0, count);
    return (ssize_t)count;
}
static ssize_t zero_write(vnode_t *vp, const void *buf, size_t count, int64_t offset) {
    (void)vp; (void)buf; (void)offset; return (ssize_t)count;
}
static int zero_lseek(vnode_t *vp, int64_t offset, int whence) {
    (void)vp; (void)offset; (void)whence; return 0;
}
static int zero_poll(vnode_t *vp, int events) {
    (void)vp;
    return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

static struct vnode_ops dev_zero_ops = {
    .open  = zero_open,
    .close = zero_close,
    .read  = zero_read,
    .write = zero_write,
    .lseek = zero_lseek,
    .poll  = zero_poll,
};

static int console_open(vnode_t *vp, int mode) {
    (void)vp; (void)mode;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    if (t) tty_open(t, mode);
    return 0;
}
static int console_close(vnode_t *vp) {
    (void)vp;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    if (t) tty_close(t);
    return 0;
}
static ssize_t console_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    (void)vp; (void)offset;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    if (!t) return 0;
    return tty_read(t, buf, count);
}
static ssize_t console_write(vnode_t *vp, const void *buf, size_t count, int64_t offset) {
    (void)vp; (void)offset;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    if (!t) return 0;
    return tty_write(t, buf, count);
}
static int console_lseek(vnode_t *vp, int64_t offset, int whence) {
    (void)vp; (void)offset; (void)whence; return -ESPIPE;
}
static int console_poll(vnode_t *vp, int events) {
    (void)vp;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    return tty_poll(t, events);
}
static waitq_t *console_poll_waitq(vnode_t *vp) {
    (void)vp;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    return tty_poll_waitq(t);
}
static int console_ioctl(vnode_t *vp, int cmd, void *data) {
    (void)vp;
    tty_t *t = tty_lookup(TTY_CONSOLE);
    if (!t) return -ENXIO;
    return tty_ioctl(t, cmd, data);
}

struct vnode_ops dev_console_ops = {
    .open  = console_open,
    .close = console_close,
    .read  = console_read,
    .write = console_write,
    .lseek = console_lseek,
    .poll  = console_poll,
    .poll_waitq = console_poll_waitq,
    .ioctl = console_ioctl,
};

typedef struct {
    const char *name;
    int  ino;
    int  type;
    int  minor;
    struct vnode_ops *ops;
} devfs_entry_t;

static devfs_entry_t devfs_entries[] = {
    {"/",       DEVFS_ROOT,    VDIR, 0,   NULL},
    {"null",    DEVFS_NULL,    VCHR, 0,   &dev_null_ops},
    {"zero",    DEVFS_ZERO,    VCHR, 0,   &dev_zero_ops},
    {"console", DEVFS_CONSOLE, VCHR, 0,   &dev_console_ops},
};
#define DEVFS_ENTRIES (sizeof(devfs_entries) / sizeof(devfs_entries[0]))

static vnode_t *devfs_dir_lookup(vnode_t *vp, const char *name) {
    (void)vp;
    if (!name) return NULL;

    for (unsigned int i = 1; i < DEVFS_ENTRIES; i++) {
        if (strcmp(devfs_entries[i].name, name) == 0) {
            vnode_t *child = vnode_alloc();
            if (!child) return NULL;
            child->type = devfs_entries[i].type;
            child->ino  = devfs_entries[i].ino;
            child->minor = devfs_entries[i].minor;
            child->ops  = devfs_entries[i].ops;
            child->refcount = 1;
            return child;
        }
    }
    return NULL;
}

static int devfs_dir_getdents(vnode_t *vp, void *buf, size_t count, int64_t *off) {
    (void)vp;
    dirent_t *out = (dirent_t *)buf;
    size_t filled = 0;
    int64_t i = *off;
    if (i < 1)
        i = 1;
    while ((uint64_t)i < DEVFS_ENTRIES) {
        if (filled + sizeof(dirent_t) > count)
            break;
        dirent_t *e = (dirent_t *)(void *)((uint8_t *)out + filled);
        e->d_ino = (uint64_t)devfs_entries[i].ino;
        e->d_off = i + 1;
        e->d_reclen = (uint16_t)sizeof(dirent_t);
        e->d_type = (devfs_entries[i].type == VDIR) ? DT_DIR : DT_CHR;
        strcpy(e->d_name, devfs_entries[i].name);
        filled += sizeof(dirent_t);
        i++;
    }
    *off = i;
    return (int)filled;
}

static struct vnode_ops devfs_dir_ops = {
    .lookup   = devfs_dir_lookup,
    .getdents = devfs_dir_getdents,
};

void devfs_init(void) {
    log_print(LOG_LEVEL_DEBUG, "devfs: init\r\n");
}

vnode_t *devfs_get_root(void) {
    vnode_t *vp = vnode_alloc();
    if (!vp) return NULL;
    vp->type = VDIR;
    vp->ino  = DEVFS_ROOT;
    vp->ops  = &devfs_dir_ops;
    vp->refcount = 1;
    return vp;
}
