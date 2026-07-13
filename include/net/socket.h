#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdint.h>
#include "lwip/tcp.h"
#include "lwip/udp.h"

struct process;

typedef enum {
    SOCKET_STATE_CLOSED,
    SOCKET_STATE_CONNECTING,
    SOCKET_STATE_CONNECTED,
    SOCKET_STATE_LISTENING,
} socket_state_t;

typedef struct {
    int domain;
    int type;
    int protocol;
    socket_state_t state;
    
    union {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
    } pcb;

    /* For blocking synchronization */
    struct process *waiter;
    int error;

    /* Receive buffer */
    uint8_t  recv_buf[4096];
    uint32_t recv_head;
    uint32_t recv_tail;

    /* For blocking recv: user buffer to fill from callback */
    void    *recv_user_buf;
    uint32_t recv_user_len;
} socket_t;

socket_t *net_socket_create(int domain, int type, int protocol);
int net_socket_connect(socket_t *s, uint32_t ip, uint16_t port);
int net_socket_bind(socket_t *s, uint32_t ip, uint16_t port);
int net_socket_listen(socket_t *s, int backlog);
int net_socket_accept(socket_t *s, uint32_t *client_ip, uint16_t *client_port);
int net_socket_send(socket_t *s, const void *buf, uint32_t len, int flags);
int net_socket_recv(socket_t *s, void *buf, uint32_t len, int flags);
void net_socket_close(socket_t *s);

struct sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

#endif
