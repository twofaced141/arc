/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/include/bsd/socket.h — BSD socket API for Arc (lwIP NO_SYS raw backend)
 *
 * When lwIP is built with NO_SYS=1 and LWIP_SOCKET=0 the upstream
 * lwip sockets header is disabled.  This header provides the same
 * constants (AF, SOCK, SOL, SO) for the in-tree socket.c and for
 * userspace (user programs include it via -I bsd/include).  Values match
 * lwIP and POSIX where practical so wire-format structs are ABI compatible.
 */

#ifndef BSD_SOCKET_H
#define BSD_SOCKET_H

#include <stdint.h>
#include <stddef.h>

/* Address families */
#define AF_UNSPEC   0
#define AF_INET     2
#define AF_INET6    10
#define PF_UNSPEC   AF_UNSPEC
#define PF_INET     AF_INET
#define PF_INET6    AF_INET6

/* Socket types */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

/* Protocols */
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_RAW     255

/* Socket level */
#define SOL_SOCKET  0xfff

/* Socket options — must match lwIP's so_options bits where relevant */
#define SO_DEBUG        0x0001
#define SO_ACCEPTCONN   0x0002
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_DONTROUTE    0x0010
#define SO_BROADCAST    0x0020
#define SO_USELOOPBACK  0x0040
#define SO_LINGER       0x0080
#define SO_REUSEPORT    0x0200
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDLOWAT     0x1003
#define SO_RCVLOWAT     0x1004
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008
#define SO_BINDTODEVICE 0x100b
#define SO_NO_CHECK     0x100a

/* shutdown(2) how */
#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

/* send/recv flags */
#define MSG_PEEK      0x01
#define MSG_WAITALL   0x02
#define MSG_OOB       0x04
#define MSG_DONTWAIT  0x08
#define MSG_MORE      0x10
#define MSG_NOSIGNAL  0x20
#define MSG_TRUNC     0x04
#define MSG_CTRUNC    0x08

/* fcntl O_NONBLOCK is defined in bsd/vfs.h — re-export for socket code */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif

/* linger */
struct linger {
    int l_onoff;
    int l_linger;
};

typedef uint8_t  sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t socklen_t;

/* in_addr is also defined by lwIP lwip/inet.h; guard to avoid redefinition. */
#ifndef LWIP_HDR_INET_H
struct in_addr {
    uint32_t s_addr; /* network byte order */
};
#endif

struct sockaddr {
    uint8_t     sa_len;
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    uint8_t     sin_len;
    sa_family_t sin_family;
    in_port_t   sin_port;   /* network byte order */
    struct in_addr sin_addr;
    char        sin_zero[8];
};

struct sockaddr_storage {
    uint8_t     s2_len;
    sa_family_t ss_family;
    char        s2_data1[2];
    uint32_t    s2_data2[3];
};

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    int           msg_iovlen;
    void         *msg_control;
    socklen_t     msg_controllen;
    int           msg_flags;
};

struct cmsghdr {
    socklen_t cmsg_len;
    int       cmsg_level;
    int       cmsg_type;
};

/* ifreq for ioctl SIOCGIF* */
#define IFNAMSIZ 6
struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_dstaddr;
        struct sockaddr ifr_broadaddr;
        struct sockaddr ifr_netmask;
        int             ifr_flags;
        int             ifr_mtu;
    } ifr_ifru;
};
#define ifr_addr      ifr_ifru.ifr_addr
#define ifr_dstaddr   ifr_ifru.ifr_dstaddr
#define ifr_broadaddr ifr_ifru.ifr_broadaddr
#define ifr_netmask   ifr_ifru.ifr_netmask
#define ifr_flags     ifr_ifru.ifr_flags
#define ifr_mtu       ifr_ifru.ifr_mtu

/* Interface flags (subset, maps to lwIP NETIF_FLAG_*) */
#define IFF_UP          0x01
#define IFF_BROADCAST   0x02
#define IFF_LOOPBACK    0x08
#define IFF_RUNNING     0x40

/* Ioctl numbers — keep small private range; also handle lwIP FIONREAD/BIO */
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFNETMASK  0x891b
#define SIOCSIFNETMASK  0x891c
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFMTU      0x8921
#define SIOCGIFCONF     0x8912

/* FIONREAD / FIONBIO — must match lwIP's _IOR/_IOW encoding */
#ifndef FIONREAD
#define FIONREAD  0x4004667fUL
#endif
#ifndef FIONBIO
#define FIONBIO   0x8004667eUL
#endif

/* netif helper declarations (implemented in bsd/net/if.c) */
struct netif;
struct netif *netif_find_by_name(const char *name);
int net_ioctl(int cmd, void *data);

#endif /* BSD_SOCKET_H */
