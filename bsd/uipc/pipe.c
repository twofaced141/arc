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


/* pipe.c — Unix pipes (uipc).  A pipe is a bounded circular buffer
 * with two vnode endpoints (read end / write end) sharing a pipe_t.
 *
 * Semantics: reads block while empty and a writer exists; writes block
 * while full and a reader exists; read returns 0 (EOF) once all write
 * ends are closed; write of a closed pipe raises SIGPIPE and returns
 * -EPIPE.  Both ends carry a vnode reference per open fd, so fork/dup
 * share the pipe correctly and the last close on a side signals EOF.
 */

#include "bsd/vfs.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/signal.h"
#include "bsd/select.h"
#include "bsd/stat.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"

#define PIPE_BUF_SIZE 4096

typedef struct pipe {
    spinlock_t lock;
    uint8_t    buf[PIPE_BUF_SIZE];
    size_t     head;         /* next write position (total bytes written) */
    size_t     tail;         /* next read position */
    int        readers;      /* open read ends */
    int        writers;      /* open write ends */
    int        freed;        /* pipe_t released (both vnodes gone) */
    waitq_t    rq;           /* blocked readers */
    waitq_t    wq;           /* blocked writers */
} pipe_t;

static size_t pipe_used(pipe_t *pp) {
    return pp->head - pp->tail;
}

static size_t pipe_space(pipe_t *pp) {
    return PIPE_BUF_SIZE - pipe_used(pp);
}

static pipe_t *pipe_get(vnode_t *vp) {
    return (pipe_t *)vp->data;
}


static int pipe_open(vnode_t *vp, int mode) {
    pipe_t *pp = pipe_get(vp);
    if (!pp)
        return -ENXIO;
    uint32_t flags;
    spin_lock_irqsave(&pp->lock, &flags);
    if (vp->ino == 0)        /* read end */
        pp->readers++;
    else                     /* write end */
        pp->writers++;
    spin_unlock_irqrestore(&pp->lock, flags);
    return 0;
}

static int pipe_close(vnode_t *vp) {
    pipe_t *pp = pipe_get(vp);
    if (!pp)
        return 0;
    uint32_t flags;
    spin_lock_irqsave(&pp->lock, &flags);
    if (vp->ino == 0)
        pp->readers--;
    else
        pp->writers--;
    int no_readers = (pp->readers <= 0);
    int no_writers = (pp->writers <= 0);
    spin_unlock_irqrestore(&pp->lock, flags);

    if (no_writers)
        waitq_wake_all(&pp->rq);   /* readers see EOF */
    if (no_readers)
        waitq_wake_all(&pp->wq);   /* writers see EPIPE */

    /* Both vnodes destroyed — free the shared state. */
    if (pp->readers <= 0 && pp->writers <= 0 && !pp->freed) {
        pp->freed = 1;
        kfree(pp);
        vp->data = NULL;
    }
    return 0;
}


static ssize_t pipe_read(vnode_t *vp, void *buf, size_t count,
                         int64_t offset) {
    (void)offset;
    pipe_t *pp = pipe_get(vp);
    if (!pp)
        return -EINVAL;

    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&pp->lock, &flags);
        size_t used = pipe_used(pp);
        int no_writers = (pp->writers <= 0);
        spin_unlock_irqrestore(&pp->lock, flags);

        if (used > 0)
            break;
        if (no_writers)
            return 0;    /* EOF */

        /* Interrupted by a signal: report a restartable syscall so the
         * SA_RESTART machinery re-runs the read after the handler. */
        int rc = waitq_sleep(&pp->rq);
        if (rc < 0)
            return -ERESTARTSYS;
    }

    size_t n = count;
    size_t used = pipe_used(pp);
    if (n > used)
        n = used;

    uint32_t flags;
    spin_lock_irqsave(&pp->lock, &flags);
    for (size_t i = 0; i < n; i++)
        ((char *)buf)[i] = pp->buf[(pp->tail + i) % PIPE_BUF_SIZE];
    pp->tail += n;
    spin_unlock_irqrestore(&pp->lock, flags);

    waitq_wake_all(&pp->wq);
    return (ssize_t)n;
}

static ssize_t pipe_write(vnode_t *vp, const void *buf, size_t count,
                          int64_t offset) {
    (void)offset;
    pipe_t *pp = pipe_get(vp);
    if (!pp)
        return -EINVAL;

    size_t written = 0;
    const char *src = (const char *)buf;

    while (written < count) {
        uint32_t flags;
        spin_lock_irqsave(&pp->lock, &flags);
        size_t space = pipe_space(pp);
        int no_readers = (pp->readers <= 0);
        spin_unlock_irqrestore(&pp->lock, flags);

        if (no_readers) {
            /* SIGPIPE unless the process ignores/handles it. */
            proc_t *p = proc_current();
            if (p && p->signals.handler[SIGPIPE] == SIG_DFL)
                p->signals.pending[SIGPIPE] = 1;
            return written > 0 ? (ssize_t)written : -EPIPE;
        }

        if (space == 0) {
            int rc = waitq_sleep(&pp->wq);
            if (rc < 0)
                return written > 0 ? (ssize_t)written : -ERESTARTSYS;
            continue;
        }

        size_t n = count - written;
        if (n > space)
            n = space;
        spin_lock_irqsave(&pp->lock, &flags);
        for (size_t i = 0; i < n; i++)
            pp->buf[(pp->head + i) % PIPE_BUF_SIZE] = src[written + i];
        pp->head += n;
    spin_unlock_irqrestore(&pp->lock, flags);
    written += n;

    waitq_wake_all(&pp->rq);
    }

    return (ssize_t)written;
}

static int pipe_lseek(vnode_t *vp, int64_t offset, int whence) {
    (void)vp; (void)offset; (void)whence;
    return -ESPIPE;
}

static int pipe_stat(vnode_t *vp, void *statbuf) {
    pipe_t *pp = pipe_get(vp);
    struct stat *st = (struct stat *)statbuf;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFIFO | 0600;
    st->st_nlink = 1;
    if (pp)
        st->st_size = (int64_t)pipe_used(pp);
    return 0;
}

static int pipe_ioctl(vnode_t *vp, int cmd, void *data) {
    (void)vp; (void)cmd; (void)data;
    return -ENOTTY;
}

static int pipe_poll(vnode_t *vp, int events) {
    pipe_t *pp = pipe_get(vp);
    int rev = 0;
    if (!pp)
        return POLLERR;

    uint32_t flags;
    spin_lock_irqsave(&pp->lock, &flags);
    int is_read_end = (vp->ino == 0);
    if (is_read_end) {
        if (pipe_used(pp) > 0 || pp->writers <= 0)
            rev |= POLLIN | POLLRDNORM;
        if (pp->writers <= 0)
            rev |= POLLHUP;
    } else {
        if (pipe_space(pp) > 0 || pp->readers <= 0)
            rev |= POLLOUT | POLLWRNORM;
        if (pp->readers <= 0)
            rev |= POLLERR;
    }
    spin_unlock_irqrestore(&pp->lock, flags);
    return rev & events;
}

static waitq_t *pipe_poll_waitq(vnode_t *vp) {
    pipe_t *pp = pipe_get(vp);
    if (!pp)
        return NULL;
    return (vp->ino == 0) ? &pp->rq : &pp->wq;
}

static struct vnode_ops pipe_read_ops = {
    .open    = pipe_open,
    .close   = pipe_close,
    .read    = pipe_read,
    .write   = pipe_write,
    .lseek   = pipe_lseek,
    .stat    = pipe_stat,
    .ioctl   = pipe_ioctl,
    .poll    = pipe_poll,
    .poll_waitq = pipe_poll_waitq,
};

static struct vnode_ops pipe_write_ops = {
    .open    = pipe_open,
    .close   = pipe_close,
    .read    = pipe_read,
    .write   = pipe_write,
    .lseek   = pipe_lseek,
    .stat    = pipe_stat,
    .ioctl   = pipe_ioctl,
    .poll    = pipe_poll,
    .poll_waitq = pipe_poll_waitq,
};


/* Create a pipe: fills *fds[2] with the read and write descriptors.
 * Returns 0 on success. */
int pipe_create(proc_t *p, int fds[2]) {
    pipe_t *pp = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!pp)
        return -ENOMEM;
    memset(pp, 0, sizeof(pipe_t));
    pp->lock = SPINLOCK_INIT;
    waitq_init(&pp->rq);
    waitq_init(&pp->wq);

    vnode_t *r = vnode_alloc();
    vnode_t *w = vnode_alloc();
    if (!r || !w) {
        kfree(pp);
        if (r) kfree(r);
        if (w) kfree(w);
        return -ENOMEM;
    }
    r->type = VFIFO;
    w->type = VFIFO;
    r->ino = 0;                 /* read end */
    w->ino = 1;                 /* write end */
    r->ops = &pipe_read_ops;
    w->ops = &pipe_write_ops;
    r->data = pp;
    w->data = pp;
    r->refcount = 1;
    w->refcount = 1;
    r->size = 0;
    w->size = 0;
    /* Both ends are open from birth: the creating fds hold the refs.
     * pipe_close frees pp when the last end is closed. */
    pp->readers = 1;
    pp->writers = 1;

    int fr = proc_fd_alloc(p);
    if (fr < 0) {
        vnode_unref(r);
        vnode_unref(w);
        return -EMFILE;
    }
    int fw = proc_fd_alloc(p);
    if (fw < 0) {
        p->fds[fr].used = 0;
        vnode_unref(r);
        vnode_unref(w);
        return -EMFILE;
    }

    p->fds[fr].vnode_ptr = (void *)r;
    p->fds[fr].flags     = O_RDONLY;
    p->fds[fr].offset    = 0;
    p->fds[fr].mode      = 0;
    p->fds[fr].used      = 1;

    p->fds[fw].vnode_ptr = (void *)w;
    p->fds[fw].flags     = O_WRONLY;
    p->fds[fw].offset    = 0;
    p->fds[fw].mode      = 0;
    p->fds[fw].used      = 1;

    fds[0] = fr;
    fds[1] = fw;
    return 0;
}
