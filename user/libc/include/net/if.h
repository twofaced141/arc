#ifndef _NET_IF_H
#define _NET_IF_H

#include <sys/types.h>
#include <sys/socket.h>

#define IFNAMSIZ 16

struct ifmap {
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char  irq;
    unsigned char  dma;
    unsigned char  port;
};

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_dstaddr;
        struct sockaddr ifr_broadaddr;
        struct sockaddr ifr_netmask;
        struct sockaddr ifr_hwaddr;
        short           ifr_flags;
        int             ifr_ifindex;
        int             ifr_metric;
        int             ifr_mtu;
        int             ifr_qlen;
        char           *ifr_data;
        char            ifr_newname[IFNAMSIZ];
        struct ifmap    ifr_map;
    };
};

struct ifconf {
    int                 ifc_len;
    union {
        void           *ifc_buf;
        struct ifreq    *ifc_req;
    };
};

#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_DEBUG       0x4
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_NOTRAILERS  0x20
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_ALLMULTI    0x200
#define IFF_MASTER      0x400
#define IFF_SLAVE       0x800
#define IFF_MULTICAST   0x1000
#define IFF_PORTSEL     0x2000
#define IFF_AUTOMEDIA   0x4000
#define IFF_DYNAMIC     0x8000

/* Standard SIOC constants */
#define SIOCGIFNAME      0x8910
#define SIOCSIFLINK      0x8911
#define SIOCGIFCONF      0x8912
#define SIOCGIFFLAGS     0x8913
#define SIOCSIFFLAGS     0x8914
#define SIOCGIFADDR      0x8915
#define SIOCSIFADDR      0x8916
#define SIOCGIFDSTADDR   0x8917
#define SIOCSIFDSTADDR   0x8918
#define SIOCGIFBRDADDR   0x8919
#define SIOCSIFBRDADDR   0x891a
#define SIOCGIFNETMASK   0x891b
#define SIOCSIFNETMASK   0x891c
#define SIOCGIFMETRIC    0x891d
#define SIOCSIFMETRIC    0x891e
#define SIOCGIFMTU       0x8921
#define SIOCSIFMTU       0x8922
#define SIOCSIFNAME      0x8923
#define SIOCSIFHWADDR    0x8924
#define SIOCGIFHWADDR    0x8927
#define SIOCGIFINDEX     0x8933
#define SIOCDIFADDR      0x8936
#define SIOCGIFTXQLEN    0x8942
#define SIOCSIFTXQLEN    0x8943
#define SIOCGIFMAP       0x8970
#define SIOCSIFMAP       0x8971
#define SIOCDEVPRIVATE   0x89F0

#endif
