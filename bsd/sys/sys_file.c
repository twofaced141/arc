#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/vfs.h"
#include "bsd/stat.h"
#include "bsd/select.h"
#include "bsd/arch.h"
#include "bsd/signal.h"
#include "bsd/time.h"
#include "vmm.h"
#include "scheduler.h"
#include "clockevent.h"
#include "debug.h"
#include "string.h"

#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))
#define ARG3(r) ((uint64_t)bsd_syscall_arg2(r))
#define ARG4(r) ((uint64_t)bsd_syscall_arg3(r))
#define ARG5(r) ((uint64_t)bsd_syscall_arg4(r))

int64_t sys_open(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    int flags = (int)ARG2(r);
    int mode  = (int)ARG3(r);
    return vfs_open(p, path, flags, mode);
}

int64_t sys_close(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    return vfs_close(p, fd);
}

int64_t sys_read(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    void *buf = (void *)ARG2(r);
    size_t count = (size_t)ARG3(r);
    if (count == 0) return 0;
    return vfs_read(p, fd, buf, count);
}

int64_t sys_write(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    const void *buf = (const void *)ARG2(r);
    size_t count = (size_t)ARG3(r);
    if (count == 0) return 0;
    return vfs_write(p, fd, buf, count);
}

int64_t sys_lseek(proc_t *p, registers_t *r) {
    int fd     = (int)ARG1(r);
    int64_t offset = (int64_t)ARG2(r);
    int whence = (int)ARG3(r);
    return vfs_lseek(p, fd, offset, whence);
}

int64_t sys_ioctl(proc_t *p, registers_t *r) {
    int fd   = (int)ARG1(r);
    int cmd  = (int)ARG2(r);
    void *data = (void *)ARG3(r);
    return vfs_ioctl(p, fd, cmd, data);
}

int64_t sys_unlink(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    return vfs_unlink(p, path);
}

int64_t sys_stat(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    struct stat *st  = (struct stat *)ARG2(r);
    struct stat kst;
    int ret = vfs_stat(p, path, &kst);
    if (ret < 0) return ret;
    if (copy_to_user(st, &kst, sizeof(kst)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_fstat(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    struct stat *st = (struct stat *)ARG2(r);
    struct stat kst;
    int ret = vfs_fstat(p, fd, &kst);
    if (ret < 0) return ret;
    if (copy_to_user(st, &kst, sizeof(kst)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_lstat(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    struct stat *st  = (struct stat *)ARG2(r);
    struct stat kst;
    int ret = vfs_lstat(p, path, &kst);
    if (ret < 0) return ret;
    if (copy_to_user(st, &kst, sizeof(kst)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_pread(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    void *buf = (void *)ARG2(r);
    size_t count = (size_t)ARG3(r);
    int64_t offset = (int64_t)ARG4(r);
    if (count == 0) return 0;
    return vfs_pread(p, fd, buf, count, offset);
}

int64_t sys_pwrite(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    const void *buf = (const void *)ARG2(r);
    size_t count = (size_t)ARG3(r);
    int64_t offset = (int64_t)ARG4(r);
    if (count == 0) return 0;
    return vfs_pwrite(p, fd, buf, count, offset);
}

int64_t sys_fcntl(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    int cmd = (int)ARG2(r);
    uint64_t arg = ARG3(r);

    switch (cmd) {
    case F_DUPFD: {
        /* Duplicate fd to the lowest free slot >= arg (POSIX). */
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= FD_MAX)
            return -EINVAL;

        filedesc_t *sf = proc_fd_get(p, fd);
        if (!sf || !sf->used)
            return -EBADF;
        int src_cloexec = sf->cloexec;

        for (int i = minfd; i < FD_MAX; i++) {
            filedesc_t *f = proc_fd_get(p, i);
            if (f && f->used)
                continue;
            int r = proc_fd_dup2(p, p, fd, i);
            if (r < 0)
                return r;
            /* F_DUPFD preserves FD_CLOEXEC (dup2 would clear it). */
            filedesc_t *nf = proc_fd_get(p, r);
            if (nf)
                nf->cloexec = src_cloexec;
            return r;
        }
        return -EMFILE;
    }
    case F_GETFD: {
        filedesc_t *f = proc_fd_get(p, fd);
        if (!f || !f->used) return -EBADF;
        return f->cloexec ? FD_CLOEXEC : 0;
    }
    case F_SETFD: {
        filedesc_t *f = proc_fd_get(p, fd);
        if (!f || !f->used) return -EBADF;
        f->cloexec = (arg & FD_CLOEXEC) ? 1 : 0;
        return 0;
    }
    case F_GETFL: {
        filedesc_t *f = proc_fd_get(p, fd);
        if (!f || !f->used) return -EBADF;
        return f->flags;
    }
    case F_SETFL: {
        filedesc_t *f = proc_fd_get(p, fd);
        if (!f || !f->used) return -EBADF;
        /* Only the status flags may be changed via fcntl (POSIX). */
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) |
                   ((int)arg & (O_APPEND | O_NONBLOCK));
        return 0;
    }
    default:
        return -EINVAL;
    }
}

int64_t sys_mkdir(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    int mode = (int)ARG2(r);
    return vfs_mkdir(p, path, mode);
}

int64_t sys_rmdir(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    return vfs_rmdir(p, path);
}

int64_t sys_symlink(proc_t *p, registers_t *r) {
    const char *target  = (const char *)ARG1(r);
    const char *linkpath = (const char *)ARG2(r);
    return vfs_symlink(p, target, linkpath);
}

int64_t sys_link(proc_t *p, registers_t *r) {
    const char *oldpath = (const char *)ARG1(r);
    const char *newpath = (const char *)ARG2(r);
    return vfs_link(p, oldpath, newpath);
}

int64_t sys_readlink(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    char *buf = (char *)ARG2(r);
    size_t buflen = (size_t)ARG3(r);
    char kbuf[1024];
    if (buflen > sizeof(kbuf)) buflen = sizeof(kbuf);

    int ret = vfs_readlink(p, path, kbuf, buflen);
    if (ret < 0) return ret;
    if (copy_to_user(buf, kbuf, (uint32_t)ret) < 0)
        return -EFAULT;
    return ret;
}

int64_t sys_rename(proc_t *p, registers_t *r) {
    const char *oldpath = (const char *)ARG1(r);
    const char *newpath = (const char *)ARG2(r);
    return vfs_rename(p, oldpath, newpath);
}

int64_t sys_mount(proc_t *p, registers_t *r) {
    const char *devpath = (const char *)ARG1(r);
    const char *path    = (const char *)ARG2(r);
    return vfs_mount(p, devpath, path);
}

int64_t sys_umount(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    return vfs_umount(p, path);
}

int64_t sys_dup2(proc_t *p, registers_t *r) {
    int oldfd = (int)ARG1(r);
    int newfd = (int)ARG2(r);
    return vfs_dup2(p, oldfd, newfd);
}

int64_t sys_pipe(proc_t *p, registers_t *r) {
    int *ufds = (int *)ARG1(r);
    int kfds[2];
    int ret = pipe_create(p, kfds);
    if (ret < 0)
        return ret;
    if (copy_to_user(ufds, kfds, sizeof(kfds)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_chmod(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    int mode = (int)ARG2(r);
    return vfs_chmod(p, path, mode);
}

int64_t sys_chown(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    int uid = (int)ARG2(r);
    int gid = (int)ARG3(r);
    return vfs_chown(p, path, uid, gid);
}

int64_t sys_umask(proc_t *p, registers_t *r) {
    int mask = (int)ARG1(r);
    int old = (int)p->umask;
    p->umask = (uint32_t)(mask & 0777);
    return old;
}

int64_t sys_access(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    int mode = (int)ARG2(r);
    return vfs_access(p, path, mode);
}

int64_t sys_truncate(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    int64_t length = (int64_t)ARG2(r);
    return vfs_truncate(p, path, length);
}

int64_t sys_ftruncate(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    int64_t length = (int64_t)ARG2(r);
    return vfs_ftruncate(p, fd, length);
}

int64_t sys_fsync(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    return vfs_fsync(p, fd);
}

int64_t sys_getdents(proc_t *p, registers_t *r) {
    int fd = (int)ARG1(r);
    void *buf = (void *)ARG2(r);
    size_t count = (size_t)ARG3(r);
    if (count == 0)
        return 0;

    void *kbuf = kmalloc((uint32_t)count);
    if (!kbuf)
        return -ENOMEM;
    int ret = vfs_getdents(p, fd, kbuf, count);
    if (ret > 0) {
        if (copy_to_user(buf, kbuf, (uint32_t)ret) < 0) {
            kfree(kbuf);
            return -EFAULT;
        }
    }
    kfree(kbuf);
    return ret;
}

int64_t sys_statvfs(proc_t *p, registers_t *r) {
    const char *path = (const char *)ARG1(r);
    void *stbuf = (void *)ARG2(r);
    struct statvfs kst;
    int ret = vfs_statvfs(p, path, &kst);
    if (ret < 0)
        return ret;
    if (copy_to_user(stbuf, &kst, sizeof(kst)) < 0)
        return -EFAULT;
    return 0;
}

/* ---- select / poll ---- */

/* POSIX: on EINTR the timeout is updated with the remaining time. */
static void select_update_timeout(struct timeval *utv, struct timeval *ktv,
                                  uint64_t deadline) {
    uint64_t now = clockevent_get_ticks();
    uint32_t left = (uint32_t)deadline - (uint32_t)now;
    if ((int32_t)left <= 0) {
        ktv->tv_sec = 0;
        ktv->tv_usec = 0;
    } else {
        ktv->tv_sec = (int64_t)(left / 100);
        ktv->tv_usec = (long)((left % 100) * 10000);
    }
    copy_to_user(utv, ktv, sizeof(*ktv));
}

int64_t sys_select(proc_t *p, registers_t *r) {
    int nfds = (int)ARG1(r);
    fd_set_t *uread  = (fd_set_t *)ARG2(r);
    fd_set_t *uwrite = (fd_set_t *)ARG3(r);
    fd_set_t *uexc   = (fd_set_t *)ARG4(r);
    struct timeval *utv = (struct timeval *)ARG5(r);

    if (nfds < 0 || nfds > FD_SETSIZE)
        return -EINVAL;

    fd_set_t kread, kwrite, kexc;
    FD_ZERO(&kread);
    FD_ZERO(&kwrite);
    FD_ZERO(&kexc);
    if (uread  && copy_from_user(&kread,  uread,  sizeof(fd_set_t)) < 0) return -EFAULT;
    if (uwrite && copy_from_user(&kwrite, uwrite, sizeof(fd_set_t)) < 0) return -EFAULT;
    if (uexc   && copy_from_user(&kexc,   uexc,   sizeof(fd_set_t)) < 0) return -EFAULT;

    /* 100 Hz tick clock: 1 tick = 10 ms.  A NULL timeout blocks
     * indefinitely; {0,0} polls once and returns immediately. */
    struct timeval ktv;
    int have_timeout = 1;
    uint64_t deadline = 0;
    if (utv) {
        if (copy_from_user(&ktv, utv, sizeof(ktv)) < 0)
            return -EFAULT;
        if (ktv.tv_sec < 0 || ktv.tv_usec < 0 || ktv.tv_usec >= 1000000)
            return -EINVAL;
        if (ktv.tv_sec == 0 && ktv.tv_usec == 0) {
            have_timeout = 0;
        } else {
            deadline = clockevent_get_ticks() +
                       (uint64_t)ktv.tv_sec * 100 +
                       (uint64_t)ktv.tv_usec / 10000;
        }
    } else {
        /* No timeout: block "forever".  The scheduler stores the
         * deadline as uint32 and wakes when (int32)(now-dl) >= 0, so
         * the offset must stay within half the 32-bit tick range
         * (~248 days at 100 Hz). */
        deadline = clockevent_get_ticks() + 0x7FFFFFFFUL;
    }

    /* Block on the waitq of the first watched descriptor that provides
     * one (e.g. a pipe read/write end): activity there wakes us early.
     * Otherwise fall back to the process waitq (signal/deadline
     * wakeups only). */
    waitq_t *wq = &p->waitq;
    if (have_timeout) {
        for (int fd = 0; fd < nfds; fd++) {
            if (!FD_ISSET(fd, &kread) && !FD_ISSET(fd, &kwrite) &&
                !FD_ISSET(fd, &kexc))
                continue;
            waitq_t *fwq = vfs_fd_poll_waitq(p, fd);
            if (fwq) {
                wq = fwq;
                break;
            }
        }
    }

    int nready = 0;
    fd_set_t rleft, wleft, eleft;
    for (;;) {
        if (signal_has_pending(p)) {
            if (utv && have_timeout)
                select_update_timeout(utv, &ktv, deadline);
            return -EINTR;
        }

        /* Working copies rebuilt each iteration: descriptors that prove
         * not-ready are cleared here so the returned sets hold only the
         * ready ones, but a re-poll after a wakeup must re-check every
         * watched descriptor. */
        rleft = kread; wleft = kwrite; eleft = kexc;
        nready = 0;
        for (int fd = 0; fd < nfds; fd++) {
            if (FD_ISSET(fd, &rleft)) {
                if (vfs_fd_poll(p, fd, POLLIN | POLLRDNORM) & (POLLIN | POLLRDNORM))
                    nready++;
                else
                    FD_CLR(fd, &rleft);
            }
            if (FD_ISSET(fd, &wleft)) {
                if (vfs_fd_poll(p, fd, POLLOUT | POLLWRNORM) & (POLLOUT | POLLWRNORM))
                    nready++;
                else
                    FD_CLR(fd, &wleft);
            }
            if (FD_ISSET(fd, &eleft)) {
                if (vfs_fd_poll(p, fd, POLLERR) & POLLERR)
                    nready++;
                else
                    FD_CLR(fd, &eleft);
            }
        }
        if (nready)
            break;
        if (!have_timeout)
            break;      /* {0,0}: poll once */
        if ((int32_t)((uint32_t)clockevent_get_ticks() - (uint32_t)deadline) >= 0)
            break;      /* timeout elapsed */

        int wret = waitq_sleep_timeout(wq, deadline);
        if (wret < 0) {
            if (utv && have_timeout)
                select_update_timeout(utv, &ktv, deadline);
            return -EINTR;
        }
        /* Woken early (descriptor ready) or by the deadline: re-check. */
    }

    if (uread  && copy_to_user(uread,  &rleft, sizeof(fd_set_t)) < 0) return -EFAULT;
    if (uwrite && copy_to_user(uwrite, &wleft, sizeof(fd_set_t)) < 0) return -EFAULT;
    if (uexc   && copy_to_user(uexc,   &eleft, sizeof(fd_set_t)) < 0) return -EFAULT;
    return nready;
}

int64_t sys_poll(proc_t *p, registers_t *r) {
    struct pollfd *ufds = (struct pollfd *)ARG1(r);
    int nfds = (int)ARG2(r);
    int timeout_ms = (int)ARG3(r);

    if (nfds < 0)
        return -EINVAL;
    if (nfds == 0) {
        if (timeout_ms < 0)
            return 0;
        return 0;
    }

    struct pollfd *kfds = (struct pollfd *)kmalloc((uint32_t)nfds * sizeof(struct pollfd));
    if (!kfds)
        return -ENOMEM;
    if (copy_from_user(kfds, ufds, (uint32_t)nfds * sizeof(struct pollfd)) < 0) {
        kfree(kfds);
        return -EFAULT;
    }

    uint64_t start = clockevent_get_ticks();
    int ret = 0;
    for (;;) {
        if (signal_has_pending(p)) {
            ret = -EINTR;
            break;
        }

        int nready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            if (kfds[i].fd < 0)
                continue;
            int rev = vfs_fd_poll(p, kfds[i].fd, kfds[i].events);
            kfds[i].revents = (short)(rev & kfds[i].events);
            if (kfds[i].revents)
                nready++;
        }
        if (nready) {
            ret = nready;
            break;
        }
        if (timeout_ms == 0) {
            ret = 0;
            break;
        }
        if (timeout_ms > 0 &&
            clockevent_get_ticks() - start >= (uint64_t)timeout_ms / 10) {
            ret = 0;
            break;
        }
        thread_yield();
    }

    if (copy_to_user(ufds, kfds, (uint32_t)nfds * sizeof(struct pollfd)) < 0)
        ret = -EFAULT;
    kfree(kfds);
    return ret;
}