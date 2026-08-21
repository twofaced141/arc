/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/lwipopts.h — lwIP configuration for ARC
 *
 * NO_SYS=1: ARC provides its own timers/threads (clockevent + scheduler),
 * so lwIP runs in NO_SYS mode (raw API, tcpip_thread disabled).
 * Memory: use lwIP mem/memp pools backed by ARC kmalloc/pmm.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include <stdint.h>

/* ---------- NO_SYS / threading ---------- */
#define NO_SYS                  1
#define LWIP_TIMERS             1
#define LWIP_TIMERS_CUSTOM      0

/* ---------- Memory ---------- */
#define MEM_ALIGNMENT           4
#define MEM_SIZE                (64 * 1024)   /* heap for mem_malloc */
#define MEMP_NUM_PBUF           32
#define MEMP_NUM_UDP_PCB        8
#define MEMP_NUM_TCP_PCB        8
#define MEMP_NUM_TCP_SEG        16
#define MEMP_NUM_NETBUF         8
#define MEMP_NUM_NETCONN        8
#define MEMP_NUM_RAW_PCB        4

/* ---------- PBUF ---------- */
#define PBUF_POOL_SIZE          32
#define PBUF_POOL_BUFSIZE       1520

/* ---------- Network ---------- */
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ARP                1
#define IP_FORWARD              0
#define LWIP_ICMP               1
#define LWIP_DHCP               1
#define LWIP_DNS                1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_RAW                1

/* ---------- TCP ---------- */
#define TCP_MSS                 1460
#define TCP_WND                 (8 * TCP_MSS)
#define TCP_SND_BUF             (8 * TCP_MSS)
#define TCP_SND_QUEUELEN        16
#define LWIP_TCP_KEEPALIVE      1

/* ---------- Checksums ---------- */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1

/* ---------- Socket / API ---------- */
#define LWIP_SOCKET             0   /* BSD socket API is in bsd/net/socket.c, not lwIP */
#define LWIP_NETCONN            0
#define LWIP_NETIF_API          0

/* ---------- Debug ---------- */
#define LWIP_DEBUG              0
#define LWIP_STATS              0
#define LWIP_STATS_DISPLAY      0

/* ---------- ARC glue ---------- */
/* freestanding: no rand(), use sys_now() pseudo-random */
uint32_t sys_now(void);
#define LWIP_RAND()             ((uint32_t)sys_now())

#endif /* LWIPOPTS_H */
