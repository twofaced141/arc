#include "net/socket.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "memory.h"
#include "debug.h"
#include "scheduler.h"
#include "process.h"
#include <string.h>

static err_t socket_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    socket_t *s = (socket_t *)arg;
    if (err != ERR_OK) {
        s->state = SOCKET_STATE_CLOSED;
        goto wake;
    }
    if (p == NULL) {
        s->state = SOCKET_STATE_CLOSED;
        goto wake;
    }

    uint32_t space = 4096 - (s->recv_head - s->recv_tail);
    uint32_t to_copy = p->tot_len;
    if (to_copy > space) to_copy = space;

    if (to_copy > 0) {
        struct pbuf *q = p;
        uint32_t off = s->recv_head;
        uint32_t left = to_copy;
        while (q && left > 0) {
            uint32_t chunk = q->len;
            if (chunk > left) chunk = left;
            uint32_t idx = off % 4096;
            if (idx + chunk <= 4096) {
                memcpy(s->recv_buf + idx, q->payload, chunk);
            } else {
                uint32_t first = 4096 - idx;
                memcpy(s->recv_buf + idx, q->payload, first);
                memcpy(s->recv_buf, (uint8_t *)q->payload + first, chunk - first);
            }
            off += chunk;
            left -= chunk;
            q = q->next;
        }
        s->recv_head = off;
    }

    tcp_recved(pcb, to_copy);
    pbuf_free(p);

    /* If a process is blocked in recv(), copy data directly to its user buffer */
    if (s->waiter && s->recv_user_buf && s->waiter->page_dir) {
        process_t *p_waiter = s->waiter;
        uint32_t avail = s->recv_head - s->recv_tail;
        uint32_t to_read = s->recv_user_len < avail ? s->recv_user_len : avail;

        if (to_read > 0) {
            uint32_t old_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
            __asm__ volatile("mov %0, %%cr3" : : "r"((uint32_t)p_waiter->page_dir));

            for (uint32_t i = 0; i < to_read; i++)
                ((uint8_t *)s->recv_user_buf)[i] = s->recv_buf[(s->recv_tail + i) % 4096];

            __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3));

            s->recv_tail += to_read;
        }

        s->recv_user_buf = NULL;
        s->recv_user_len = 0;

        registers_t *frame = (registers_t *)p_waiter->kernel_esp;
        frame->eax = (int)to_read;
    }

wake:
    if (s->waiter) {
        scheduler_unblock_process(s->waiter);
        s->waiter = NULL;
    }
    return ERR_OK;
}

static void socket_tcp_err(void *arg, err_t err) {
    (void)err;
    socket_t *s = (socket_t *)arg;
    s->state = SOCKET_STATE_CLOSED;
    s->error = err;
    if (s->waiter) {
        /* Overwrite saved eax with -1 to indicate error */
        registers_t *frame = (registers_t *)s->waiter->kernel_esp;
        frame->eax = -1;
        scheduler_unblock_process(s->waiter);
        s->waiter = NULL;
    }
}

static err_t socket_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    socket_t *s = (socket_t *)arg;
    if (err != ERR_OK) {
        socket_tcp_err(arg, err);
        return ERR_OK;
    }
    s->state = SOCKET_STATE_CONNECTED;
    s->error = err;
    if (s->waiter) {
        registers_t *frame = (registers_t *)s->waiter->kernel_esp;
        frame->eax = 0;
        scheduler_unblock_process(s->waiter);
        s->waiter = NULL;
    }
    return ERR_OK;
}

static err_t socket_tcp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    socket_t *s = (socket_t *)arg;
    if (err != ERR_OK || !newpcb) return ERR_OK;

    s->state = SOCKET_STATE_CONNECTED;
    s->pcb.tcp = newpcb;
    tcp_arg(newpcb, s);
    tcp_recv(newpcb, socket_tcp_recv);
    tcp_err(newpcb, socket_tcp_err);

    if (s->waiter) {
        registers_t *frame = (registers_t *)s->waiter->kernel_esp;
        frame->eax = 0;
        scheduler_unblock_process(s->waiter);
        s->waiter = NULL;
    }
    return ERR_OK;
}

socket_t *net_socket_create(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    if (type != SOCK_STREAM) return NULL;

    socket_t *s = (socket_t *)kmalloc(sizeof(socket_t));
    if (!s) return NULL;

    memset(s, 0, sizeof(socket_t));
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->state = SOCKET_STATE_CLOSED;
    s->pcb.tcp = tcp_new();
    if (!s->pcb.tcp) {
        kfree(s);
        return NULL;
    }

    tcp_arg(s->pcb.tcp, s);
    tcp_recv(s->pcb.tcp, socket_tcp_recv);
    tcp_err(s->pcb.tcp, socket_tcp_err);

    s->waiter = NULL;
    s->error = 0;
    s->recv_head = 0;
    s->recv_tail = 0;
    s->recv_user_buf = NULL;
    s->recv_user_len = 0;

    return s;
}

int net_socket_bind(socket_t *s, uint32_t ip, uint16_t port) {
    if (s->state != SOCKET_STATE_CLOSED) return -1;

    ip_addr_t addr;
    addr.addr = ip;

    err_t err = tcp_bind(s->pcb.tcp, &addr, port);
    if (err != ERR_OK) return -1;
    return 0;
}

int net_socket_listen(socket_t *s, int backlog) {
    (void)backlog;
    if (s->state != SOCKET_STATE_CLOSED) return -1;

    struct tcp_pcb *lpcb = tcp_listen_with_backlog(s->pcb.tcp, 1);
    if (!lpcb) return -1;

    s->pcb.tcp = lpcb;
    s->state = SOCKET_STATE_LISTENING;

    tcp_arg(lpcb, s);
    tcp_accept(lpcb, socket_tcp_accept);
    return 0;
}

int net_socket_accept(socket_t *s, uint32_t *client_ip, uint16_t *client_port) {
    if (s->state != SOCKET_STATE_LISTENING) return -1;

    /* tcp_accept callback already set up new connection.
     * After accept callback fires, state becomes CONNECTED.
     * If no connection yet, block and wait. */
    if (s->state == SOCKET_STATE_LISTENING) {
        s->waiter = scheduler_current_process();
        return -2; /* would block — caller should set PROC_BLOCKED */
    }

    if (client_ip) *client_ip = s->pcb.tcp->remote_ip.addr;
    if (client_port) *client_port = s->pcb.tcp->remote_port;
    return 0;
}

int net_socket_connect(socket_t *s, uint32_t ip, uint16_t port) {
    if (s->state != SOCKET_STATE_CLOSED) return -1;

    ip_addr_t addr;
    addr.addr = ip;

    s->state = SOCKET_STATE_CONNECTING;
    s->waiter = scheduler_current_process();

    err_t err = tcp_connect(s->pcb.tcp, &addr, port, socket_tcp_connected);
    if (err != ERR_OK) {
        s->state = SOCKET_STATE_CLOSED;
        s->waiter = NULL;
        return -1;
    }

    return -2; /* would block */
}

int net_socket_send(socket_t *s, const void *buf, uint32_t len, int flags) {
    (void)flags;
    if (s->state != SOCKET_STATE_CONNECTED) return -1;
    err_t err = tcp_write(s->pcb.tcp, buf, len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) return -1;
    tcp_output(s->pcb.tcp);
    return (int)len;
}

int net_socket_recv(socket_t *s, void *buf, uint32_t len, int flags) {
    (void)flags;
    uint32_t avail = s->recv_head - s->recv_tail;

    if (s->state == SOCKET_STATE_CLOSED) {
        if (avail > 0) goto copy_data;
        return -1;
    }

    if (avail > 0) goto copy_data;

    /* No data — block and wait for callback */
    s->waiter = scheduler_current_process();
    s->recv_user_buf = buf;
    s->recv_user_len = len;
    return -2; /* would block */

copy_data:;
    uint32_t to_read = len < avail ? len : avail;
    for (uint32_t i = 0; i < to_read; i++)
        ((uint8_t *)buf)[i] = s->recv_buf[(s->recv_tail + i) % 4096];
    s->recv_tail += to_read;
    return (int)to_read;
}

void net_socket_close(socket_t *s) {
    if (!s) return;
    s->waiter = NULL;
    s->recv_user_buf = NULL;
    s->recv_user_len = 0;
    if (s->pcb.tcp) {
        tcp_arg(s->pcb.tcp, NULL);
        tcp_recv(s->pcb.tcp, NULL);
        tcp_err(s->pcb.tcp, NULL);
        tcp_accept(s->pcb.tcp, NULL);
        tcp_close(s->pcb.tcp);
    }
    kfree(s);
}
