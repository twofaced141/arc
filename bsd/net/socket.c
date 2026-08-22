/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/socket.c — BSD socket layer for Arc (lwIP NO_SYS=1 raw API)
 *
 * Domain: AF_INET only (LWIP_IPV6 disabled in lwipopts.h).
 * Uses lwIP raw API (tcp/udp/raw pcbs) — no netconn, no lwIP sockets.
 * Each socket is a VSOCK vnode; its vnode->data points to arc_sock.
 * Blocking semantics are built on waitq + signal interruption.
 */

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/timeouts.h"

#include "bsd/socket.h"
#include "bsd/vfs.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/select.h"
#include "bsd/signal.h"
#include "bsd/syscall.h"
#include "bsd/arch.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"
#include "vmm.h"         /* copy_from_user / copy_to_user, kmalloc */

/* ------------------------------------------------------------------ */
/* errno mapping                                                       */

static int err_to_errno(err_t err) {
    switch (err) {
    case ERR_OK:          return 0;
    case ERR_MEM:         return -ENOMEM;
    case ERR_BUF:         return -ENOMEM;
    case ERR_TIMEOUT:     return -ETIMEDOUT;
    case ERR_RTE:         return -ENETUNREACH;
    case ERR_INPROGRESS:  return -EINPROGRESS;
    case ERR_VAL:         return -EINVAL;
    case ERR_WOULDBLOCK:  return -EAGAIN;
    case ERR_USE:         return -EADDRINUSE;
    case ERR_ALREADY:     return -EALREADY;
    case ERR_ISCONN:      return -EISCONN;
    case ERR_CONN:        return -ENOTCONN;
    case ERR_IF:          return -ENETDOWN;
    case ERR_ABRT:        return -ECONNABORTED;
    case ERR_RST:         return -ECONNRESET;
    case ERR_CLSD:        return -ECONNRESET;
    case ERR_ARG:         return -EINVAL;
    default:              return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* arc_sock definition                                                 */

#define ARC_SOCK_UNCONNECTED  0
#define ARC_SOCK_BOUND        1
#define ARC_SOCK_LISTENING    2
#define ARC_SOCK_CONNECTING   3
#define ARC_SOCK_CONNECTED    4
#define ARC_SOCK_CLOSED       5

struct udp_msg {
    struct pbuf *p;
    ip_addr_t addr;
    u16_t port;
    struct udp_msg *next;
};

struct arc_sock {
    int domain;
    int type;
    int protocol;
    int state;
    int err;               /* pending error for SO_ERROR */
    int so_options;        /* SO_REUSEADDR etc */
    int nonblock;
    int shut_rd;
    int shut_wr;

    union {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
        struct raw_pcb *raw;
    } pcb;
    struct tcp_pcb *listen_pcb; /* for listening sockets */

    /* generic waitq — used by poll/select and blocking ops */
    waitq_t waitq;

    /* TCP rx queue */
    struct pbuf *rx_head;
    struct pbuf *rx_tail;
    size_t rx_off;   /* offset inside rx_head */
    size_t rx_total;
    int tcp_closed;  /* peer closed (p==NULL received) */

    /* listen accept queue */
    struct arc_sock *accept_head;
    struct arc_sock *accept_tail;
    struct arc_sock *accept_next;
    int backlog;
    int accept_len;

    /* UDP rx queue */
    struct udp_msg *udp_head;
    struct udp_msg *udp_tail;
    int udp_len;

    /* connect synchronisation */
    int connecting;
    int connect_err;

    spinlock_t lock;
    vnode_t *vnode;
};

static struct vnode_ops sock_ops;

/* forward decls */
static void sock_wake(struct arc_sock *s);
static int sock_is_readable(struct arc_sock *s);
static int sock_is_writable(struct arc_sock *s);

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void sock_wake(struct arc_sock *s) {
    if (!s) return;
    waitq_wake_all(&s->waitq);
}

static int sock_is_readable(struct arc_sock *s) {
    if (!s) return 0;
    if (s->err) return 1;
    if (s->type == SOCK_STREAM) {
        if (s->state == ARC_SOCK_LISTENING) return s->accept_len > 0;
        if (s->rx_total > 0) return 1;
        if (s->tcp_closed) return 1;
        return 0;
    } else if (s->type == SOCK_DGRAM) {
        return s->udp_len > 0;
    } else if (s->type == SOCK_RAW) {
        return s->rx_total > 0;
    }
    return 0;
}

static int sock_is_writable(struct arc_sock *s) {
    if (!s) return 0;
    if (s->shut_wr) return 0;
    if (s->err) return 1;
    if (s->type == SOCK_STREAM) {
        if (s->state == ARC_SOCK_CONNECTING) return 0;
        if (s->state != ARC_SOCK_CONNECTED && s->state != ARC_SOCK_BOUND && s->state != ARC_SOCK_UNCONNECTED) {
            /* listening sockets are not writable */
            if (s->state == ARC_SOCK_LISTENING) return 0;
        }
        if (s->pcb.tcp) {
            return tcp_sndbuf(s->pcb.tcp) > 0;
        }
        return 0;
    }
    /* UDP/RAW are always writable unless error */
    return 1;
}

/* sockaddr helpers — callers must have validated user memory already */
static int sockaddr_to_ip4(const struct sockaddr *sa, socklen_t len, ip_addr_t *out, u16_t *port) {
    if (!sa || !out) return -EINVAL;
    if (len < sizeof(struct sockaddr_in)) return -EINVAL;
    if (sa->sa_family != AF_INET) return -EAFNOSUPPORT;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    /* sin_len is optional; ignore */
    out->addr = sin->sin_addr.s_addr;
    if (port) *port = lwip_ntohs(sin->sin_port);
    return 0;
}

static void ip4_to_sockaddr(const ip_addr_t *ip, u16_t port, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_len = sizeof(*out);
    out->sin_family = AF_INET;
    out->sin_port = lwip_htons(port);
    if (ip) out->sin_addr.s_addr = ip->addr;
    else out->sin_addr.s_addr = IPADDR_ANY;
}

/* ------------------------------------------------------------------ */
/* TCP callbacks                                                       */

static void tcp_error_cb(void *arg, err_t err) {
    struct arc_sock *s = (struct arc_sock *)arg;
    if (!s) return;
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    s->err = err_to_errno(err) ? -err_to_errno(err) : -ECONNRESET;
    if (s->err > 0) s->err = -s->err;
    s->state = ARC_SOCK_CLOSED;
    s->connecting = 0;
    s->connect_err = s->err;
    if (s->pcb.tcp) {
        s->pcb.tcp = NULL;
    }
    spin_unlock_irqrestore(&s->lock, flags);
    sock_wake(s);
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    struct arc_sock *s = (struct arc_sock *)arg;
    (void)tpcb; (void)err;
    if (!s) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    if (!p) {
        /* peer closed */
        s->tcp_closed = 1;
        spin_unlock_irqrestore(&s->lock, flags);
        sock_wake(s);
        return ERR_OK;
    }
    /* chain pbuf */
    if (!s->rx_head) {
        s->rx_head = s->rx_tail = p;
    } else {
        pbuf_cat(s->rx_tail, p);
        /* pbuf_cat updates tot_len etc; tail remains head chain */
        /* find new tail */
        struct pbuf *q = s->rx_head;
        while (q->next) q = q->next;
        s->rx_tail = q;
    }
    s->rx_total += p->tot_len;
    /* tell lwIP we will handle flow control via tcp_recved */
    spin_unlock_irqrestore(&s->lock, flags);
    sock_wake(s);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)tpcb; (void)len;
    struct arc_sock *s = (struct arc_sock *)arg;
    sock_wake(s);
    return ERR_OK;
}

static err_t tcp_poll_cb(void *arg, struct tcp_pcb *tpcb) {
    (void)arg; (void)tpcb;
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    struct arc_sock *s = (struct arc_sock *)arg;
    if (!s) return ERR_OK;
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    s->connecting = 0;
    if (err != ERR_OK) {
        s->connect_err = err_to_errno(err);
        s->err = s->connect_err;
        s->state = ARC_SOCK_CLOSED;
    } else {
        s->state = ARC_SOCK_CONNECTED;
        s->connect_err = 0;
        /* setup recv/sent callbacks */
        tcp_arg(tpcb, s);
        tcp_recv(tpcb, tcp_recv_cb);
        tcp_sent(tpcb, tcp_sent_cb);
        tcp_poll(tpcb, tcp_poll_cb, 4);
        tcp_err(tpcb, tcp_error_cb);
    }
    spin_unlock_irqrestore(&s->lock, flags);
    sock_wake(s);
    return ERR_OK;
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    struct arc_sock *listen = (struct arc_sock *)arg;
    if (!listen || err != ERR_OK || !newpcb) return ERR_VAL;
    if (listen->accept_len >= listen->backlog) {
        log_printf(LOG_LEVEL_ERROR, "tcp_accept backlog full\n");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    struct arc_sock *cs = (struct arc_sock *)kmalloc(sizeof(struct arc_sock));
    if (!cs) {
        tcp_abort(newpcb);
        return ERR_MEM;
    }
    memset(cs, 0, sizeof(*cs));
    cs->domain = listen->domain;
    cs->type = SOCK_STREAM;
    cs->protocol = IPPROTO_TCP;
    cs->state = ARC_SOCK_CONNECTED;
    cs->pcb.tcp = newpcb;
    cs->lock = SPINLOCK_INIT;
    waitq_init(&cs->waitq);
    /* setup callbacks on new pcb */
    tcp_arg(newpcb, cs);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_sent(newpcb, tcp_sent_cb);
    tcp_poll(newpcb, tcp_poll_cb, 4);
    tcp_err(newpcb, tcp_error_cb);

    uint32_t flags;
    spin_lock_irqsave(&listen->lock, &flags);
    cs->accept_next = NULL;
    if (!listen->accept_head) listen->accept_head = listen->accept_tail = cs;
    else { listen->accept_tail->accept_next = cs; listen->accept_tail = cs; }
    listen->accept_len++;
    spin_unlock_irqrestore(&listen->lock, flags);
    sock_wake(listen);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* UDP / RAW callbacks                                                 */

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    struct arc_sock *s = (struct arc_sock *)arg;
    if (!s || !p) { if (p) pbuf_free(p); return; }
    struct udp_msg *m = (struct udp_msg *)kmalloc(sizeof(struct udp_msg));
    if (!m) { pbuf_free(p); return; }
    m->p = p;
    m->addr = *addr;
    m->port = port;
    m->next = NULL;
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    if (!s->udp_head) s->udp_head = s->udp_tail = m;
    else { s->udp_tail->next = m; s->udp_tail = m; }
    s->udp_len++;
    s->rx_total += p->tot_len;
    spin_unlock_irqrestore(&s->lock, flags);
    sock_wake(s);
}

static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)pcb; (void)addr;
    struct arc_sock *s = (struct arc_sock *)arg;
    if (!s || !p) { if (p) pbuf_free(p); return 0; }
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    if (!s->rx_head) s->rx_head = s->rx_tail = p;
    else {
        pbuf_cat(s->rx_tail, p);
        struct pbuf *q = s->rx_head; while (q->next) q=q->next; s->rx_tail=q;
    }
    s->rx_total += p->tot_len;
    spin_unlock_irqrestore(&s->lock, flags);
    sock_wake(s);
    return 0; /* eat packet */
}

/* ------------------------------------------------------------------ */
/* vnode ops                                                           */

static int sock_stat(vnode_t *vp, void *statbuf) {
    struct stat *st = (struct stat *)statbuf;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFSOCK | 0777;
    st->st_nlink = 1;
    st->st_ino = vp ? (uint32_t)vp->ino : 0;
    struct arc_sock *s = vp ? (struct arc_sock *)vp->data : NULL;
    if (s) st->st_size = (int64_t)s->rx_total;
    return 0;
}

static int sock_ioctl(vnode_t *vp, int cmd, void *data) {
    struct arc_sock *s = vp ? (struct arc_sock *)vp->data : NULL;
    if (!s) return -ENOTTY;
    switch (cmd) {
    case FIONREAD: {
        int avail = 0;
        uint32_t flags;
        spin_lock_irqsave(&s->lock, &flags);
        if (s->type == SOCK_STREAM || s->type == SOCK_RAW) avail = (int)s->rx_total;
        else if (s->type == SOCK_DGRAM) {
            if (s->udp_head) avail = (int)s->udp_head->p->tot_len;
        }
        spin_unlock_irqrestore(&s->lock, flags);
        if (copy_to_user(data, &avail, sizeof(avail)) < 0) return -EFAULT;
        return 0;
    }
    case FIONBIO: {
        int on;
        if (copy_from_user(&on, data, sizeof(on)) < 0) return -EFAULT;
        uint32_t flags;
        spin_lock_irqsave(&s->lock, &flags);
        s->nonblock = on ? 1 : 0;
        spin_unlock_irqrestore(&s->lock, flags);
        /* also update fd flags for caller's fds that point to this vnode */
        proc_t *p = proc_current();
        if (p) {
            for (int i=0;i<p->fd_capacity;i++) {
                filedesc_t *f = proc_fd_get(p, i);
                if (f && f->used && f->vnode_ptr == vp) {
                    if (on) f->flags |= O_NONBLOCK;
                    else f->flags &= ~O_NONBLOCK;
                }
            }
        }
        return 0;
    }
    default:
        /* delegate to netif ioctls for SIOCGIF* */
        if (cmd == SIOCGIFADDR || cmd == SIOCSIFADDR ||
            cmd == SIOCGIFNETMASK || cmd == SIOCSIFNETMASK ||
            cmd == SIOCGIFFLAGS || cmd == SIOCSIFFLAGS ||
            cmd == SIOCGIFMTU) {
            /* data is struct ifreq in kernel address? ioctl from vfs passes user pointer?
             * vfs_ioctl passes data pointer as-is from syscall (user pointer).
             * For SIOCGIF* we need to copy ifreq from user, operate, copy back.
             */
            struct ifreq kifr;
            if (copy_from_user(&kifr, data, sizeof(kifr)) < 0) return -EFAULT;
            int r = net_ioctl(cmd, &kifr);
            if (r == 0 && (cmd == SIOCGIFADDR || cmd == SIOCGIFNETMASK || cmd == SIOCGIFFLAGS || cmd == SIOCGIFMTU)) {
                if (copy_to_user(data, &kifr, sizeof(kifr)) < 0) return -EFAULT;
            }
            return r;
        }
        return -ENOTTY;
    }
}

static int sock_poll(vnode_t *vp, int events) {
    struct arc_sock *s = vp ? (struct arc_sock *)vp->data : NULL;
    if (!s) return POLLERR;
    int rev = 0;
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    int rd = sock_is_readable(s);
    int wr = sock_is_writable(s);
    if (s->err) rev |= POLLERR;
    if (s->tcp_closed) rev |= POLLHUP;
    spin_unlock_irqrestore(&s->lock, flags);
    if ((events & (POLLIN|POLLRDNORM)) && rd) rev |= (events & (POLLIN|POLLRDNORM));
    if ((events & (POLLOUT|POLLWRNORM)) && wr) rev |= (events & (POLLOUT|POLLWRNORM));
    return rev & events;
}

static waitq_t *sock_poll_waitq(vnode_t *vp) {
    struct arc_sock *s = vp ? (struct arc_sock *)vp->data : NULL;
    return s ? &s->waitq : NULL;
}

static ssize_t sock_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    (void)offset;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (!s) return -EINVAL;
    if (s->shut_rd) return 0;
    if (count == 0) return 0;

    /* For datagram sockets, read is recvfrom with no address */
    if (s->type == SOCK_DGRAM) {
        /* dequeue one datagram, truncated to count */
        for (;;) {
            uint32_t flags;
            spin_lock_irqsave(&s->lock, &flags);
            struct udp_msg *m = s->udp_head;
            if (m) {
                size_t n = m->p->tot_len;
                if (n > count) n = count;
                spin_unlock_irqrestore(&s->lock, flags);
                /* copy out of pbuf chain */
                pbuf_copy_partial(m->p, buf, (u16_t)n, 0);
                spin_lock_irqsave(&s->lock, &flags);
                s->udp_head = m->next;
                if (!s->udp_head) s->udp_tail = NULL;
                s->udp_len--;
                s->rx_total -= m->p->tot_len;
                spin_unlock_irqrestore(&s->lock, flags);
                pbuf_free(m->p);
                kfree(m);
                return (ssize_t)n;
            }
            int nb = s->nonblock;
            int closed = 0; /* UDP never closed */
            spin_unlock_irqrestore(&s->lock, flags);
            if (nb) return -EAGAIN;
            int rc = waitq_sleep(&s->waitq);
            if (rc < 0) return -ERESTARTSYS;
            if (signal_has_pending(proc_current())) return -ERESTARTSYS;
            (void)closed;
        }
    }

    /* STREAM / RAW: stream read from rx queue */
    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&s->lock, &flags);
        if (s->err) { int e = s->err; spin_unlock_irqrestore(&s->lock, flags); return e; }
        if (s->rx_head) { spin_unlock_irqrestore(&s->lock, flags); break; }
        if (s->tcp_closed) { spin_unlock_irqrestore(&s->lock, flags); return 0; }
        int nb = s->nonblock;
        spin_unlock_irqrestore(&s->lock, flags);
        if (nb) return -EAGAIN;
        int rc = waitq_sleep(&s->waitq);
        if (rc < 0) return -ERESTARTSYS;
        if (signal_has_pending(proc_current())) return -ERESTARTSYS;
    }

    /* copy from pbuf chain respecting rx_off */
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    size_t copied = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (copied < count && s->rx_head) {
        struct pbuf *p = s->rx_head;
        size_t avail = p->tot_len - s->rx_off;
        size_t need = count - copied;
        size_t take = avail < need ? avail : need;
        spin_unlock_irqrestore(&s->lock, flags);
        pbuf_copy_partial(p, dst + copied, (u16_t)take, (u16_t)s->rx_off);
        spin_lock_irqsave(&s->lock, &flags);
        copied += take;
        s->rx_off += take;
        s->rx_total -= take;
        if (s->rx_off >= p->tot_len) {
            /* consumed whole chain */
            s->rx_head = NULL;
            s->rx_tail = NULL;
            s->rx_off = 0;
            spin_unlock_irqrestore(&s->lock, flags);
            pbuf_free(p);
            if (s->type == SOCK_STREAM && s->pcb.tcp) {
                tcp_recved(s->pcb.tcp, (u16_t)take);
            }
            sock_wake(s);
            break;
        }
    }
    spin_unlock_irqrestore(&s->lock, flags);
    return (ssize_t)copied;
}

static ssize_t sock_write(vnode_t *vp, const void *buf, size_t count, int64_t offset) {
    (void)offset;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (!s) return -EINVAL;
    if (s->shut_wr) return -EPIPE;
    if (count == 0) return 0;
    if (s->err) return s->err;

    if (s->type == SOCK_DGRAM) {
        /* need destination: use connected peer if any */
        if (!s->pcb.udp) return -ENOTCONN;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)count, PBUF_RAM);
        if (!p) return -ENOMEM;
        pbuf_take(p, buf, (u16_t)count);
        err_t e;
        uint32_t flags;
        spin_lock_irqsave(&s->lock, &flags);
        /* if pcb is connected, udp_send else use last? For now connected only */
        e = udp_send(s->pcb.udp, p);
        spin_unlock_irqrestore(&s->lock, flags);
        pbuf_free(p);
        if (e != ERR_OK) return err_to_errno(e);
        return (ssize_t)count;
    }

    if (s->type == SOCK_STREAM) {
        if (s->state != ARC_SOCK_CONNECTED) return -ENOTCONN;
        if (!s->pcb.tcp) return -ENOTCONN;
        size_t written = 0;
        const uint8_t *src = (const uint8_t *)buf;
        while (written < count) {
            uint32_t flags;
            spin_lock_irqsave(&s->lock, &flags);
            struct tcp_pcb *tpcb = s->pcb.tcp;
            if (!tpcb) { spin_unlock_irqrestore(&s->lock, flags); return -ENOTCONN; }
            u16_t sndbuf = tcp_sndbuf(tpcb);
            if (sndbuf == 0) {
                int nb = s->nonblock;
                spin_unlock_irqrestore(&s->lock, flags);
                if (nb) return written ? (ssize_t)written : -EAGAIN;
                int rc = waitq_sleep(&s->waitq);
                if (rc < 0) return written ? (ssize_t)written : -ERESTARTSYS;
                continue;
            }
            size_t chunk = count - written;
            if (chunk > sndbuf) chunk = sndbuf;
            if (chunk > TCP_MSS) chunk = TCP_MSS;
            err_t e = tcp_write(tpcb, src + written, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_MEM) {
                /* need to flush */
                spin_unlock_irqrestore(&s->lock, flags);
                tcp_output(tpcb);
                if (s->nonblock) return written ? (ssize_t)written : -EAGAIN;
                int rc = waitq_sleep(&s->waitq);
                if (rc < 0) return written ? (ssize_t)written : -ERESTARTSYS;
                continue;
            }
            if (e != ERR_OK) {
                spin_unlock_irqrestore(&s->lock, flags);
                return err_to_errno(e);
            }
            written += chunk;
            spin_unlock_irqrestore(&s->lock, flags);
            tcp_output(tpcb);
        }
        return (ssize_t)written;
    }

    if (s->type == SOCK_RAW) {
        if (!s->pcb.raw) return -ENOTCONN;
        struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)count, PBUF_RAM);
        if (!p) return -ENOMEM;
        pbuf_take(p, buf, (u16_t)count);
        err_t e = raw_send(s->pcb.raw, p);
        pbuf_free(p);
        if (e != ERR_OK) return err_to_errno(e);
        return (ssize_t)count;
    }
    return -EOPNOTSUPP;
}

static int sock_close(vnode_t *vp) {
    struct arc_sock *s = vp ? (struct arc_sock *)vp->data : NULL;
    if (!s) return 0;
    /* abort pcbs */
    uint32_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    if (s->type == SOCK_STREAM) {
        if (s->listen_pcb) {
            tcp_close(s->listen_pcb);
            s->listen_pcb = NULL;
        }
        if (s->pcb.tcp) {
            tcp_arg(s->pcb.tcp, NULL);
            tcp_recv(s->pcb.tcp, NULL);
            tcp_sent(s->pcb.tcp, NULL);
            tcp_err(s->pcb.tcp, NULL);
            tcp_poll(s->pcb.tcp, NULL, 0);
            /* try graceful close, fallback to abort if memory */
            err_t e = tcp_close(s->pcb.tcp);
            if (e != ERR_OK) tcp_abort(s->pcb.tcp);
            s->pcb.tcp = NULL;
        }
        /* free rx chain */
        if (s->rx_head) { pbuf_free(s->rx_head); s->rx_head = s->rx_tail = NULL; }
        /* free accept queue — abort pending */
        struct arc_sock *q = s->accept_head;
        while (q) {
            struct arc_sock *n = q->accept_next;
            if (q->pcb.tcp) tcp_abort(q->pcb.tcp);
            if (q->rx_head) pbuf_free(q->rx_head);
            kfree(q);
            q = n;
        }
    } else if (s->type == SOCK_DGRAM) {
        if (s->pcb.udp) { udp_remove(s->pcb.udp); s->pcb.udp = NULL; }
        struct udp_msg *m = s->udp_head;
        while (m) { struct udp_msg *n=m->next; pbuf_free(m->p); kfree(m); m=n; }
    } else if (s->type == SOCK_RAW) {
        if (s->pcb.raw) { raw_remove(s->pcb.raw); s->pcb.raw = NULL; }
        if (s->rx_head) pbuf_free(s->rx_head);
    }
    s->state = ARC_SOCK_CLOSED;
    spin_unlock_irqrestore(&s->lock, flags);
    sock_wake(s);
    kfree(s);
    vp->data = NULL;
    return 0;
}

static struct vnode_ops sock_ops = {
    .close = sock_close,
    .read  = sock_read,
    .write = sock_write,
    .stat  = sock_stat,
    .ioctl = sock_ioctl,
    .poll  = sock_poll,
    .poll_waitq = sock_poll_waitq,
};

/* ------------------------------------------------------------------ */
/* socket creation helper                                              */

static int sock_create(int domain, int type, int protocol, struct arc_sock **out) {
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) return -EPROTOTYPE;
    if (protocol == 0) {
        if (type == SOCK_STREAM) protocol = IPPROTO_TCP;
        else if (type == SOCK_DGRAM) protocol = IPPROTO_UDP;
        else protocol = IPPROTO_RAW;
    }
    if (type == SOCK_STREAM && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;
    if (type == SOCK_DGRAM && protocol != IPPROTO_UDP) return -EPROTONOSUPPORT;

    struct arc_sock *s = (struct arc_sock *)kmalloc(sizeof(struct arc_sock));
    if (!s) return -ENOMEM;
    memset(s, 0, sizeof(*s));
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->state = ARC_SOCK_UNCONNECTED;
    s->lock = SPINLOCK_INIT;
    waitq_init(&s->waitq);

    if (type == SOCK_STREAM) {
        s->pcb.tcp = tcp_new();
        if (!s->pcb.tcp) { kfree(s); return -ENOBUFS; }
        tcp_arg(s->pcb.tcp, s);
        tcp_err(s->pcb.tcp, tcp_error_cb);
    } else if (type == SOCK_DGRAM) {
        s->pcb.udp = udp_new();
        if (!s->pcb.udp) { kfree(s); return -ENOBUFS; }
        udp_recv(s->pcb.udp, udp_recv_cb, s);
    } else {
        s->pcb.raw = raw_new(protocol);
        if (!s->pcb.raw) { kfree(s); return -ENOBUFS; }
        raw_recv(s->pcb.raw, raw_recv_cb, s);
        raw_bind(s->pcb.raw, IP_ADDR_ANY);
    }
    *out = s;
    return 0;
}

int socket_set_nonblock(vnode_t *vp, int nb) {
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (!s) return -EINVAL;
    uint32_t flags; spin_lock_irqsave(&s->lock,&flags); s->nonblock = nb?1:0; spin_unlock_irqrestore(&s->lock,flags);
    return 0;
}

/* ------------------------------------------------------------------ */
/* syscall helpers — copy sockaddr from user                           */

static int copy_sockaddr_from_user(const struct sockaddr *uaddr, socklen_t ulen,
                                   struct sockaddr_storage *kaddr) {
    if (!uaddr) return -EFAULT;
    if (ulen == 0 || ulen > sizeof(*kaddr)) return -EINVAL;
    if (copy_from_user(kaddr, uaddr, ulen) < 0) return -EFAULT;
    /* basic sanity */
    if (kaddr->s2_len == 0) kaddr->s2_len = (uint8_t)ulen;
    return 0;
}

/* ------------------------------------------------------------------ */
/* exported syscalls                                                   */

int64_t sys_socket(proc_t *p, registers_t *r) {
    int domain = (int)bsd_syscall_arg0(r);
    int type   = (int)bsd_syscall_arg1(r);
    int proto  = (int)bsd_syscall_arg2(r);

    struct arc_sock *s;
    int err = sock_create(domain, type, proto, &s);
    if (err < 0) return err;

    vnode_t *vp = vnode_alloc();
    if (!vp) {
        /* cleanup pcb */
        if (s->type == SOCK_STREAM && s->pcb.tcp) tcp_abort(s->pcb.tcp);
        if (s->type == SOCK_DGRAM && s->pcb.udp) udp_remove(s->pcb.udp);
        if (s->type == SOCK_RAW && s->pcb.raw) raw_remove(s->pcb.raw);
        kfree(s);
        return -ENOMEM;
    }
    vp->type = VSOCK;
    vp->ops = &sock_ops;
    vp->data = s;
    vp->refcount = 1;
    s->vnode = vp;

    int fd = proc_fd_alloc(p);
    if (fd < 0) {
        vnode_unref(vp);
        return -EMFILE;
    }
    filedesc_t *f = proc_fd_get(p, fd);
    /* proc_fd_alloc already marks used, fill */
    p->fds[fd].vnode_ptr = vp;
    p->fds[fd].flags = 0;
    p->fds[fd].offset = 0;
    p->fds[fd].used = 1;
    (void)f;
    return fd;
}

int64_t sys_bind(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    const struct sockaddr *uaddr = (const struct sockaddr *)bsd_syscall_arg1(r);
    socklen_t ulen = (socklen_t)bsd_syscall_arg2(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (!s) return -EINVAL;
    if (s->state != ARC_SOCK_UNCONNECTED) return -EINVAL;

    struct sockaddr_storage kaddr;
    int err = copy_sockaddr_from_user(uaddr, ulen, &kaddr);
    if (err < 0) return err;
    ip_addr_t ip; u16_t port;
    err = sockaddr_to_ip4((struct sockaddr *)&kaddr, ulen, &ip, &port);
    if (err < 0) return err;

    if (s->type == SOCK_STREAM) {
        err_t e = tcp_bind(s->pcb.tcp, &ip, port);
        if (e != ERR_OK) return err_to_errno(e);
    } else if (s->type == SOCK_DGRAM) {
        err_t e = udp_bind(s->pcb.udp, &ip, port);
        if (e != ERR_OK) return err_to_errno(e);
    } else {
        err_t e = raw_bind(s->pcb.raw, &ip);
        if (e != ERR_OK) return err_to_errno(e);
    }
    uint32_t flags; spin_lock_irqsave(&s->lock,&flags); s->state = ARC_SOCK_BOUND; spin_unlock_irqrestore(&s->lock,flags);
    return 0;
}

int64_t sys_listen(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    int backlog = (int)bsd_syscall_arg1(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->state == ARC_SOCK_CONNECTED) return -EINVAL;
    if (backlog <= 0) backlog = 5;
    if (backlog > 128) backlog = 128;

    struct tcp_pcb *old = s->pcb.tcp;
    if (!old) return -EINVAL;
    struct tcp_pcb *lpcb = tcp_listen_with_backlog(old, (u8_t)backlog);
    if (!lpcb) return -ENOMEM;
    /* tcp_listen_with_backlog frees old? No, it creates new and old is freed on success? Actually it returns new lpcb and old is consumed. */
    s->pcb.tcp = NULL;
    s->listen_pcb = lpcb;
    tcp_arg(lpcb, s);
    tcp_accept(lpcb, tcp_accept_cb);
    uint32_t flags; spin_lock_irqsave(&s->lock,&flags);
    s->state = ARC_SOCK_LISTENING;
    s->backlog = backlog;
    s->accept_head = s->accept_tail = NULL;
    s->accept_len = 0;
    spin_unlock_irqrestore(&s->lock,flags);
    return 0;
}

int64_t sys_accept(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    struct sockaddr *uaddr = (struct sockaddr *)bsd_syscall_arg1(r);
    socklen_t *ulenp = (socklen_t *)bsd_syscall_arg2(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->state != ARC_SOCK_LISTENING) return -EINVAL;

    for (;;) {
        uint32_t flags; spin_lock_irqsave(&s->lock,&flags);
        struct arc_sock *cs = s->accept_head;
        if (cs) {
            s->accept_head = cs->accept_next;
            if (!s->accept_head) s->accept_tail = NULL;
            s->accept_len--;
            cs->accept_next = NULL;
            spin_unlock_irqrestore(&s->lock,flags);

            /* create new fd for accepted socket */
            vnode_t *nvp = vnode_alloc();
            if (!nvp) { kfree(cs); return -ENOMEM; }
            nvp->type = VSOCK;
            nvp->ops = &sock_ops;
            nvp->data = cs;
            nvp->refcount = 1;
            cs->vnode = nvp;
            int nfd = proc_fd_alloc(p);
            if (nfd < 0) { vnode_unref(nvp); kfree(cs); return -EMFILE; }
            p->fds[nfd].vnode_ptr = nvp;
            p->fds[nfd].flags = 0;
            p->fds[nfd].used = 1;

            if (uaddr && ulenp) {
                socklen_t klen;
                if (copy_from_user(&klen, ulenp, sizeof(klen)) == 0) {
                    /* peer address from pcb */
                    ip_addr_t *rip = NULL; u16_t rport = 0;
                    if (cs->pcb.tcp) { rip = &cs->pcb.tcp->remote_ip; rport = cs->pcb.tcp->remote_port; }
                    struct sockaddr_in sin;
                    ip4_to_sockaddr(rip, rport, &sin);
                    size_t cpy = klen < sizeof(sin) ? klen : sizeof(sin);
                    if (copy_to_user(uaddr, &sin, (uint32_t)cpy) == 0) {
                        socklen_t out = (socklen_t)sizeof(sin);
                        copy_to_user(ulenp, &out, sizeof(out));
                    }
                }
            }
            return nfd;
        }
        int nb = s->nonblock || (f->flags & O_NONBLOCK);
        spin_unlock_irqrestore(&s->lock,flags);
        if (nb) return -EAGAIN;
        int rc = waitq_sleep(&s->waitq);
        if (rc < 0) return -ERESTARTSYS;
        if (signal_has_pending(p)) return -ERESTARTSYS;
    }
}

int64_t sys_connect(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    const struct sockaddr *uaddr = (const struct sockaddr *)bsd_syscall_arg1(r);
    socklen_t ulen = (socklen_t)bsd_syscall_arg2(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (!s) return -EINVAL;
    if (s->state == ARC_SOCK_CONNECTED) return -EISCONN;
    if (s->state == ARC_SOCK_CONNECTING) return -EALREADY;

    struct sockaddr_storage kaddr;
    int err = copy_sockaddr_from_user(uaddr, ulen, &kaddr);
    if (err < 0) return err;
    ip_addr_t ip; u16_t port;
    err = sockaddr_to_ip4((struct sockaddr *)&kaddr, ulen, &ip, &port);
    if (err < 0) return err;

    if (s->type == SOCK_STREAM) {
        uint32_t flags; spin_lock_irqsave(&s->lock,&flags);
        s->connecting = 1; s->connect_err = 0; s->state = ARC_SOCK_CONNECTING;
        spin_unlock_irqrestore(&s->lock,flags);
        err_t e = tcp_connect(s->pcb.tcp, &ip, port, tcp_connected_cb);
        if (e != ERR_OK) {
            spin_lock_irqsave(&s->lock,&flags); s->connecting=0; s->state=ARC_SOCK_UNCONNECTED; spin_unlock_irqrestore(&s->lock,flags);
            return err_to_errno(e);
        }
        if (s->nonblock || (f->flags & O_NONBLOCK)) return -EINPROGRESS;
        /* block until connected or error */
        for (;;) {
            spin_lock_irqsave(&s->lock,&flags);
            int done = !s->connecting;
            int ce = s->connect_err;
            spin_unlock_irqrestore(&s->lock,flags);
            if (done) {
                if (ce) return ce;
                return 0;
            }
            int rc = waitq_sleep(&s->waitq);
            if (rc < 0) {
                /* abort connect */
                spin_lock_irqsave(&s->lock,&flags); s->connecting=0; spin_unlock_irqrestore(&s->lock,flags);
                tcp_abort(s->pcb.tcp);
                /* need new pcb after abort */
                struct tcp_pcb *npcb = tcp_new();
                if (npcb) { s->pcb.tcp = npcb; tcp_arg(npcb,s); tcp_err(npcb,tcp_error_cb); s->state=ARC_SOCK_UNCONNECTED; }
                return -ERESTARTSYS;
            }
        }
    } else if (s->type == SOCK_DGRAM) {
        err_t e = udp_connect(s->pcb.udp, &ip, port);
        if (e != ERR_OK) return err_to_errno(e);
        uint32_t flags; spin_lock_irqsave(&s->lock,&flags); s->state = ARC_SOCK_CONNECTED; spin_unlock_irqrestore(&s->lock,flags);
        return 0;
    } else {
        return -EOPNOTSUPP;
    }
}

int64_t sys_getsockname(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    struct sockaddr *uaddr = (struct sockaddr *)bsd_syscall_arg1(r);
    socklen_t *ulenp = (socklen_t *)bsd_syscall_arg2(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (!uaddr || !ulenp) return -EFAULT;
    socklen_t klen;
    if (copy_from_user(&klen, ulenp, sizeof(klen)) < 0) return -EFAULT;
    ip_addr_t ip; u16_t port = 0;
    if (s->type == SOCK_STREAM && s->pcb.tcp) { ip = s->pcb.tcp->local_ip; port = s->pcb.tcp->local_port; }
    else if (s->type == SOCK_STREAM && s->listen_pcb) { ip = s->listen_pcb->local_ip; port = s->listen_pcb->local_port; }
    else if (s->type == SOCK_DGRAM && s->pcb.udp) { ip = s->pcb.udp->local_ip; port = s->pcb.udp->local_port; }
    else ip_addr_set_any(0,&ip);
    struct sockaddr_in sin; ip4_to_sockaddr(&ip, port, &sin);
    size_t cpy = klen < sizeof(sin) ? klen : sizeof(sin);
    if (copy_to_user(uaddr, &sin, (uint32_t)cpy) < 0) return -EFAULT;
    socklen_t out = (socklen_t)sizeof(sin);
    if (copy_to_user(ulenp, &out, sizeof(out)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_getpeername(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    struct sockaddr *uaddr = (struct sockaddr *)bsd_syscall_arg1(r);
    socklen_t *ulenp = (socklen_t *)bsd_syscall_arg2(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (s->state != ARC_SOCK_CONNECTED) return -ENOTCONN;
    if (!uaddr || !ulenp) return -EFAULT;
    socklen_t klen;
    if (copy_from_user(&klen, ulenp, sizeof(klen)) < 0) return -EFAULT;
    ip_addr_t ip; u16_t port = 0;
    if (s->type == SOCK_STREAM && s->pcb.tcp) { ip = s->pcb.tcp->remote_ip; port = s->pcb.tcp->remote_port; }
    else if (s->type == SOCK_DGRAM && s->pcb.udp) { ip = s->pcb.udp->remote_ip; port = s->pcb.udp->remote_port; }
    else return -ENOTCONN;
    struct sockaddr_in sin; ip4_to_sockaddr(&ip, port, &sin);
    size_t cpy = klen < sizeof(sin) ? klen : sizeof(sin);
    if (copy_to_user(uaddr, &sin, (uint32_t)cpy) < 0) return -EFAULT;
    socklen_t out = (socklen_t)sizeof(sin);
    if (copy_to_user(ulenp, &out, sizeof(out)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_sendto(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    const void *buf = (const void *)bsd_syscall_arg1(r);
    size_t len = (size_t)bsd_syscall_arg2(r);
    int flags = (int)bsd_syscall_arg3(r);
    const struct sockaddr *uaddr = (const struct sockaddr *)bsd_syscall_arg4(r);
    socklen_t ulen = 0;
    /* ARG5 is not in registers_t for x86? Use bsd_syscall_arg4 already covers;
     * we pass socklen via stack copy — instead expect 6th arg via extra.
     * For simplicity, if uaddr != NULL we try to copy full sockaddr_in regardless of len.
     */
    (void)ulen;
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;

    /* If destination given, treat as sendto for UDP; otherwise use connected */
    if (uaddr) {
        struct sockaddr_storage kaddr;
        /* ulen unknown — assume sizeof(sockaddr_in); try copy that much */
        if (copy_from_user(&kaddr, uaddr, sizeof(struct sockaddr_in)) < 0) return -EFAULT;
        ip_addr_t ip; u16_t port;
        int err = sockaddr_to_ip4((struct sockaddr *)&kaddr, sizeof(struct sockaddr_in), &ip, &port);
        if (err < 0) return err;
        if (s->type == SOCK_DGRAM) {
            if (len > 65507) return -EMSGSIZE;
            void *kbuf = kmalloc((uint32_t)len);
            if (!kbuf) return -ENOMEM;
            if (copy_from_user(kbuf, buf, (uint32_t)len) < 0) { kfree(kbuf); return -EFAULT; }
            struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
            if (!pb) { kfree(kbuf); return -ENOMEM; }
            pbuf_take(pb, kbuf, (u16_t)len);
            kfree(kbuf);
            err_t e = udp_sendto(s->pcb.udp, pb, &ip, port);
            pbuf_free(pb);
            if (e != ERR_OK) return err_to_errno(e);
            return (int64_t)len;
        }
        /* For TCP, destination must match connected peer; ignore and just send */
    }

    /* stream send path — copy user buf to kernel then via sock_write logic */
    if (flags & MSG_DONTWAIT) {
        int prev = s->nonblock;
        s->nonblock = 1;
        void *kbuf = kmalloc((uint32_t)len);
        if (!kbuf) { s->nonblock = prev; return -ENOMEM; }
        if (copy_from_user(kbuf, buf, (uint32_t)len) < 0) { kfree(kbuf); s->nonblock=prev; return -EFAULT; }
        ssize_t ret = sock_write(vp, kbuf, len, 0);
        kfree(kbuf);
        s->nonblock = prev;
        return ret;
    }
    void *kbuf = kmalloc((uint32_t)len);
    if (!kbuf) return -ENOMEM;
    if (copy_from_user(kbuf, buf, (uint32_t)len) < 0) { kfree(kbuf); return -EFAULT; }
    ssize_t ret = sock_write(vp, kbuf, len, 0);
    kfree(kbuf);
    return ret;
}

int64_t sys_recvfrom(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    void *buf = (void *)bsd_syscall_arg1(r);
    size_t len = (size_t)bsd_syscall_arg2(r);
    int flags = (int)bsd_syscall_arg3(r);
    struct sockaddr *uaddr = (struct sockaddr *)bsd_syscall_arg4(r);
    socklen_t *ulenp = NULL; /* 6th arg handling — try to fetch from stack if needed */
    /* On amd64, 6th arg is in r9 / different; bsd_syscall_arg4 is 5th, so check extra */
    /* Use generic: if we have 6 args, try to read from user stack? Instead use convention:
     * recvfrom(fd, buf, len, flags, src, srclen) — srclen pointer is expected in high word?
     * For simplicity we ignore srclen when not provided via alternative.
     * We attempt to fetch 6th arg via bsd_syscall_arg5 if exists. */
    /* Note: bsd_arch.h may expose up to 6 args; we try */
    // try to get 6th arg via inline
    // fallback: if we cannot, we just not fill address length
    /* Use the syscall number to detect? simply attempt to read from registers if arch supports */
    /* We signal that if uaddr != NULL but we can't get ulenp, we still fill sockaddr */
    (void)ulenp;
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;

    int want_peek = (flags & MSG_PEEK) != 0;
    int dontwait = (flags & MSG_DONTWAIT) != 0;

    if (s->type == SOCK_DGRAM) {
        /* block until datagram available */
        for (;;) {
            uint32_t fl; spin_lock_irqsave(&s->lock,&fl);
            struct udp_msg *m = s->udp_head;
            if (m) {
                size_t n = m->p->tot_len;
                size_t copy = n < len ? n : len;
                spin_unlock_irqrestore(&s->lock,fl);
                void *kbuf = kmalloc((uint32_t)copy);
                if (!kbuf) return -ENOMEM;
                pbuf_copy_partial(m->p, kbuf, (u16_t)copy, 0);
                if (copy_to_user(buf, kbuf, (uint32_t)copy) < 0) { kfree(kbuf); return -EFAULT; }
                kfree(kbuf);
                if (uaddr) {
                    struct sockaddr_in sin; ip4_to_sockaddr(&m->addr, m->port, &sin);
                    /* try to get ulenp via 6th arg if available */
                    // we don't have it, just copy sockaddr regardless of len
                    if (copy_to_user(uaddr, &sin, sizeof(sin)) < 0) return -EFAULT;
                }
                if (!want_peek) {
                    spin_lock_irqsave(&s->lock,&fl);
                    s->udp_head = m->next;
                    if (!s->udp_head) s->udp_tail = NULL;
                    s->udp_len--; s->rx_total -= m->p->tot_len;
                    spin_unlock_irqrestore(&s->lock,fl);
                    pbuf_free(m->p); kfree(m);
                }
                if (n > len) return -EMSGSIZE; /* truncated but we returned copy */
                return (int64_t)copy;
            }
            int nb = s->nonblock || dontwait || (f->flags & O_NONBLOCK);
            spin_unlock_irqrestore(&s->lock,fl);
            if (nb) return -EAGAIN;
            int rc = waitq_sleep(&s->waitq);
            if (rc < 0) return -ERESTARTSYS;
        }
    }

    /* STREAM */
    /* use sock_read for stream; handle MSG_PEEK by not consuming? For simplicity peek not supported — treat as normal */
    (void)want_peek;
    if (dontwait) {
        int prev = s->nonblock; s->nonblock = 1;
        void *kbuf = kmalloc((uint32_t)len);
        if (!kbuf) { s->nonblock = prev; return -ENOMEM; }
        ssize_t ret2 = sock_read(vp, kbuf, len, 0);
        if (ret2 > 0 && copy_to_user(buf, kbuf, (uint32_t)ret2) < 0) ret2 = -EFAULT;
        kfree(kbuf); s->nonblock = prev;
        return ret2;
    }
    void *kbuf2 = kmalloc((uint32_t)len);
    if (!kbuf2) return -ENOMEM;
    ssize_t ret = sock_read(vp, kbuf2, len, 0);
    if (ret > 0) {
        if (copy_to_user(buf, kbuf2, (uint32_t)ret) < 0) ret = -EFAULT;
        if (uaddr && ret > 0) {
            /* fill peer addr */
            ip_addr_t ip; u16_t port=0;
            if (s->pcb.tcp) { ip = s->pcb.tcp->remote_ip; port = s->pcb.tcp->remote_port; }
            else ip_addr_set_any(0,&ip);
            struct sockaddr_in sin; ip4_to_sockaddr(&ip, port, &sin);
            copy_to_user(uaddr, &sin, sizeof(sin));
        }
    }
    kfree(kbuf2);
    return ret;
}

int64_t sys_setsockopt(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    int level = (int)bsd_syscall_arg1(r);
    int optname = (int)bsd_syscall_arg2(r);
    const void *optval = (const void *)bsd_syscall_arg3(r);
    socklen_t optlen = (socklen_t)bsd_syscall_arg4(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    int v = 0;
    if (optlen >= 4) { if (copy_from_user(&v, optval, 4) < 0) return -EFAULT; }
    else if (optlen > 0) { uint8_t b=0; if (copy_from_user(&b, optval,1)<0) return -EFAULT; v=b; }
    switch (optname) {
    case SO_REUSEADDR:
        s->so_options = v ? (s->so_options|SO_REUSEADDR) : (s->so_options&~SO_REUSEADDR);
        if (s->type==SOCK_STREAM && s->pcb.tcp) {
            if (v) ip_set_option(s->pcb.tcp, SOF_REUSEADDR); else ip_reset_option(s->pcb.tcp, SOF_REUSEADDR);
        }
        return 0;
    case SO_KEEPALIVE:
        s->so_options = v ? (s->so_options|SO_KEEPALIVE) : (s->so_options&~SO_KEEPALIVE);
        if (s->type==SOCK_STREAM && s->pcb.tcp) {
            if (v) s->pcb.tcp->so_options |= SOF_KEEPALIVE; else s->pcb.tcp->so_options &= ~SOF_KEEPALIVE;
        }
        return 0;
    case SO_RCVBUF:
    case SO_SNDBUF:
        /* lwIP buffers are fixed; accept but ignore */
        return 0;
    case SO_LINGER: {
        struct linger l; if (optlen < sizeof(l)) return -EINVAL;
        if (copy_from_user(&l, optval, sizeof(l))<0) return -EFAULT;
        s->so_options = l.l_onoff ? (s->so_options|SO_LINGER) : (s->so_options&~SO_LINGER);
        /* lwIP SO_LINGER handling is internal; we just remember the flag */
        return 0;
    }
    default: return -ENOPROTOOPT;
    }
}

int64_t sys_getsockopt(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    int level = (int)bsd_syscall_arg1(r);
    int optname = (int)bsd_syscall_arg2(r);
    void *optval = (void *)bsd_syscall_arg3(r);
    socklen_t *optlenp = (socklen_t *)bsd_syscall_arg4(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    if (!optval || !optlenp) return -EFAULT;
    socklen_t klen;
    if (copy_from_user(&klen, optlenp, sizeof(klen))<0) return -EFAULT;
    int v = 0;
    switch (optname) {
    case SO_TYPE: v = s->type; break;
    case SO_ERROR: v = s->err; s->err = 0; break;
    case SO_REUSEADDR: v = (s->so_options & SO_REUSEADDR)?1:0; break;
    case SO_KEEPALIVE: v = (s->so_options & SO_KEEPALIVE)?1:0; break;
    case SO_ACCEPTCONN: v = (s->state==ARC_SOCK_LISTENING)?1:0; break;
    default: return -ENOPROTOOPT;
    }
    if (klen < 4) return -EINVAL;
    if (copy_to_user(optval, &v, 4)<0) return -EFAULT;
    socklen_t out = 4;
    if (copy_to_user(optlenp, &out, sizeof(out))<0) return -EFAULT;
    return 0;
}

int64_t sys_shutdown(proc_t *p, registers_t *r) {
    int fd = (int)bsd_syscall_arg0(r);
    int how = (int)bsd_syscall_arg1(r);
    filedesc_t *f = proc_fd_get(p, fd);
    if (!f || !f->used) return -EBADF;
    vnode_t *vp = (vnode_t *)f->vnode_ptr;
    if (!vp || vp->type != VSOCK) return -ENOTSOCK;
    struct arc_sock *s = (struct arc_sock *)vp->data;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    uint32_t flags; spin_lock_irqsave(&s->lock,&flags);
    if (how==SHUT_RD || how==SHUT_RDWR) s->shut_rd=1;
    if (how==SHUT_WR || how==SHUT_RDWR) s->shut_wr=1;
    spin_unlock_irqrestore(&s->lock,flags);
    if (s->pcb.tcp) {
        if (how==SHUT_RDWR) tcp_shutdown(s->pcb.tcp,0,1);
        else if (how==SHUT_RD) tcp_shutdown(s->pcb.tcp,1,0);
        else tcp_shutdown(s->pcb.tcp,0,1);
    }
    sock_wake(s);
    return 0;
}
