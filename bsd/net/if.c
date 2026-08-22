/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/if.c — network interface layer for Arc (lwIP NO_SYS=1)
 *
 * Wraps lwIP netif list + loopback.  Ethernet drivers register via
 * netif_add() wrapper.  Ioctl SIOCGIF* / SIOCSIF* are served here.
 */

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "lwip/inet.h"
#include "lwip/def.h"

#include "bsd/socket.h"
#include "bsd/errno.h"
#include "spinlock.h"
#include "string.h"
#include "debug.h"

/* lwIP defines IFNAMSIZ / NETIF_NAMESIZE as 6, same as our socket.h */
#ifndef NETIF_NAMESIZE
#define NETIF_NAMESIZE 6
#endif

/* --------------------------------------------------------------- */
/* loopback netif                                                  */

static struct netif loop_netif;
static int loop_inited = 0;

static err_t loop_output(struct netif *netif, struct pbuf *p,
                         const ip4_addr_t *ipaddr) {
    (void)ipaddr;
    struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!q) {
            return ERR_MEM;
    }
    if (pbuf_copy(q, p) != ERR_OK) {
            pbuf_free(q);
        return ERR_MEM;
    }
    err_t err = netif->input(q, netif);
    if (err != ERR_OK) pbuf_free(q);
    return err;
}

static err_t loop_linkoutput(struct netif *netif, struct pbuf *p) {
    struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!q) return ERR_MEM;
    if (pbuf_copy(q, p) != ERR_OK) { pbuf_free(q); return ERR_MEM; }
    err_t err = netif->input(q, netif);
    if (err != ERR_OK) pbuf_free(q);
    return err;
}

static err_t loop_init(struct netif *netif) {
    netif->name[0] = 'l';
    netif->name[1] = 'o';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->hwaddr_len = 6;
    memset(netif->hwaddr, 0, 6);
    netif->output = loop_output;
    netif->linkoutput = loop_linkoutput;
    return ERR_OK;
}

/* --------------------------------------------------------------- */
/* Ethernet helper for drivers                                     */

err_t arc_ethernet_input(struct pbuf *p, struct netif *netif) {
    if (!p || !netif) return ERR_VAL;
    return ethernet_input(p, netif);
}

struct netif *arc_netif_add(struct netif *netif,
                            const ip4_addr_t *ipaddr,
                            const ip4_addr_t *netmask,
                            const ip4_addr_t *gw,
                            void *state,
                            netif_init_fn init,
                            netif_input_fn input) {
    struct netif *n = netif_add(netif, ipaddr, netmask, gw, state, init, input);
    if (n) {
        netif_set_up(n);
        netif_set_link_up(n);
        if (!netif_default) netif_set_default(n);
    }
    return n;
}

/* Called by drivers to deliver a received frame upward.
 * In NO_SYS mode this is called from driver context (irq or poll) —
 * caller must ensure it is not in interrupt with interrupts disabled
 * for long. We just forward to netif->input. */
err_t arc_net_input(struct netif *netif, struct pbuf *p) {
    if (!netif || !p) return ERR_VAL;
    return netif->input(p, netif);
}

/* --------------------------------------------------------------- */
/* net init / tick                                                 */

void net_init(void) {
    lwip_init();
    /* create loopback 127.0.0.1/8 */
    ip4_addr_t lo_ip, lo_nm, lo_gw;
    IP4_ADDR(&lo_ip, 127, 0, 0, 1);
    IP4_ADDR(&lo_nm, 255, 0, 0, 0);
    IP4_ADDR(&lo_gw, 127, 0, 0, 1);
    struct netif *n = netif_add(&loop_netif, &lo_ip, &lo_nm, &lo_gw,
                                NULL, loop_init, netif_input);
    if (n) {
        netif_set_default(n);
        netif_set_up(n);
        netif_set_link_up(n);
        loop_inited = 1;
        log_print(LOG_LEVEL_INFO, "net: loopback lo0 127.0.0.1 up\r\n");
    }
}

void net_timer_tick(void) {
    sys_check_timeouts();
#if LWIP_NETIF_LOOPBACK
    /* Poll loopback for NO_SYS. netif_loop_output queues packets,
     * they are delivered via netif_poll. */
    extern void netif_poll_all(void);
    netif_poll_all();
#endif
}

/* --------------------------------------------------------------- */
/* Helpers */

struct netif *netif_find_by_name(const char *name) {
    if (!name) return NULL;
    return netif_find((char *)name);
}

static int copy_sockaddr_in(struct sockaddr_in *dst, const ip4_addr_t *src, uint16_t port) {
    if (!dst || !src) return -EINVAL;
    memset(dst, 0, sizeof(*dst));
    dst->sin_len = sizeof(*dst);
    dst->sin_family = AF_INET;
    dst->sin_port = lwip_htons(port);
    dst->sin_addr.s_addr = src->addr; /* already network order */
    return 0;
}

static int sockaddr_to_ip(const struct sockaddr *sa, ip4_addr_t *out, uint16_t *port) {
    if (!sa || !out) return -EINVAL;
    if (sa->sa_family != AF_INET) return -EAFNOSUPPORT;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    out->addr = sin->sin_addr.s_addr;
    if (port) *port = lwip_ntohs(sin->sin_port);
    return 0;
}

/* --------------------------------------------------------------- */
/* Ioctl dispatch for sockets (called from socket ioctl)           */

int net_ioctl(int cmd, void *data) {
    struct ifreq *ifr = (struct ifreq *)data;
    if (!ifr) return -EINVAL;

    struct netif *netif = NULL;
    if (cmd == SIOCGIFCONF) {
        /* data is struct ifconf — not yet supported, return empty */
        return -ENOSYS;
    }

    /* Most commands need an interface name */
    netif = netif_find(ifr->ifr_name);
    if (!netif) return -ENODEV;

    switch (cmd) {
    case SIOCGIFADDR: {
        const ip4_addr_t *ip = netif_ip4_addr(netif);
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
        copy_sockaddr_in(sin, ip, 0);
        return 0;
    }
    case SIOCSIFADDR: {
        ip4_addr_t ip;
        uint16_t port;
        if (sockaddr_to_ip(&ifr->ifr_addr, &ip, &port) < 0) return -EINVAL;
        netif_set_ipaddr(netif, &ip);
        return 0;
    }
    case SIOCGIFNETMASK: {
        const ip4_addr_t *nm = netif_ip4_netmask(netif);
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
        copy_sockaddr_in(sin, nm, 0);
        return 0;
    }
    case SIOCSIFNETMASK: {
        ip4_addr_t nm;
        uint16_t p;
        if (sockaddr_to_ip(&ifr->ifr_addr, &nm, &p) < 0) return -EINVAL;
        netif_set_netmask(netif, &nm);
        return 0;
    }
    case SIOCGIFFLAGS: {
        int flags = 0;
        if (netif_is_up(netif)) flags |= IFF_UP;
        if (netif_is_flag_set(netif, NETIF_FLAG_BROADCAST)) flags |= IFF_BROADCAST;
        if (netif->name[0] == 'l' && netif->name[1] == 'o') flags |= IFF_LOOPBACK;
        if (netif_is_link_up(netif)) flags |= IFF_RUNNING;
        ifr->ifr_flags = flags;
        return 0;
    }
    case SIOCSIFFLAGS: {
        if (ifr->ifr_flags & IFF_UP) netif_set_up(netif);
        else netif_set_down(netif);
        return 0;
    }
    case SIOCGIFMTU: {
        ifr->ifr_mtu = netif->mtu;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}
