#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                  1
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0
#define LWIP_NETIF_API          0

#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_DHCP               1
#define LWIP_AUTOIP             0
#define LWIP_DNS                0
#define LWIP_IGMP               0
#define LWIP_ICMP               1

#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_RAW                1

#define LWIP_NO_CTYPE_H         1

#define LWIP_IPV4               1
#define LWIP_IPV6               0

#define LWIP_STATS              0
#define LWIP_HAVE_LOOPIF        0
#define LWIP_NETIF_LOOPBACK     0

#define MEM_ALIGNMENT           4
#define MEM_SIZE                65536
#define MEMP_NUM_PBUF           32
#define MEMP_NUM_UDP_PCB        8
#define MEMP_NUM_TCP_PCB        16
#define MEMP_NUM_TCP_PCB_LISTEN 8
#define MEMP_NUM_TCP_SEG        16
#define MEMP_NUM_ARP_QUEUE      8
#define MEMP_NUM_NETBUF         8
#define MEMP_NUM_NETCONN        0

#define PBUF_POOL_SIZE          16
#define PBUF_POOL_BUFSIZE       1536
#define PBUF_LINK_HLEN          16

#define TCP_SND_BUF             4096
#define TCP_WND                 4096
#define TCP_MSS                 1460
#define TCP_TTL                 64
#define TCP_SYNMAXRTX           5

#define LWIP_TCP_KEEPALIVE      0

#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_GEN_ICMP       1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1
#define CHECKSUM_CHECK_ICMP     1

#define IP_FORWARD              0
#define IP_OPTIONS_ALLOWED      0
#define IP_REASSEMBLY           0
#define IP_FRAG                 0

#endif
