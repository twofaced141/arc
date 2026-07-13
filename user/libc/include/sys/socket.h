#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>

typedef unsigned short sa_family_t;

#define __SOCKADDR_COMMON(sa_prefix) sa_family_t sa_prefix##family
#define __SOCKADDR_COMMON_SIZE (sizeof(unsigned short))

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

#define _SS_SIZE 128
#define _SS_PADSIZE (_SS_SIZE - (sizeof(unsigned short) + sizeof(unsigned long long)))

struct sockaddr_storage {
    sa_family_t ss_family;
    unsigned long long __ss_align;
    char __ss_padding[_SS_PADSIZE];
};

/* Socket types */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5
#define SOCK_DCCP      6
#define SOCK_PACKET    10

/* Protocol families */
#define PF_UNSPEC      0
#define PF_LOCAL       1
#define PF_UNIX        PF_LOCAL
#define PF_INET        2
#define PF_INET6       10
#define PF_PACKET      17

#define AF_UNSPEC      PF_UNSPEC
#define AF_LOCAL       PF_LOCAL
#define AF_UNIX        AF_LOCAL
#define AF_INET        PF_INET
#define AF_INET6       PF_INET6
#define AF_PACKET      PF_PACKET

/* Shutdown options */
#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

/* Message flags */
#define MSG_PEEK       2
#define MSG_WAITALL    0x100
#define MSG_OOB        1
#define MSG_NOSIGNAL   0x4000

/* Socket options */
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_KEEPALIVE   9
#define SO_BROADCAST   6
#define SO_LINGER      13
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_MARK        36

/* struct iovec for msghdr */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

/* struct msghdr for sendmsg/recvmsg */
struct msghdr {
    void            *msg_name;
    unsigned int     msg_namelen;
    struct iovec    *msg_iov;
    size_t           msg_iovlen;
    void            *msg_control;
    size_t           msg_controllen;
    int              msg_flags;
};

/* Ancillary data (CMSG) for ping */
struct cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_FIRSTHDR(msg) \
    ((msg)->msg_control ? \
     ((struct cmsghdr *)(msg)->msg_control) : \
     (struct cmsghdr *)0)
#define CMSG_NXTHDR(msg, cmsg) \
    ((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) >= \
     (unsigned char *)(msg)->msg_control + (msg)->msg_controllen ? \
     (struct cmsghdr *)0 : \
     (struct cmsghdr *)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_DATA(cmsg) ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))

/* Socket function declarations */
int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr *addr, unsigned int addrlen);
int connect(int sockfd, const struct sockaddr *addr, unsigned int addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, unsigned int *addrlen);
int setsockopt(int sockfd, int level, int optname, const void *optval, unsigned int optlen);
int shutdown(int sockfd, int how);
int getpeername(int sockfd, struct sockaddr *addr, unsigned int *addrlen);
int getsockname(int sockfd, struct sockaddr *addr, unsigned int *addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, unsigned int addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, unsigned int *addrlen);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

#endif
