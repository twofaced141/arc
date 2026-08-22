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


#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stddef.h>

/* Syscall numbers (Linux-ish mapping, +1024 offset in kernel dispatch) */
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_WAITPID 6
#define SYS_GETPID  7
#define SYS_BRK     9
#define SYS_NANOSLEEP 10
#define SYS_EXECVE  11
#define SYS_GETCWD  13
#define SYS_CHDIR   14
#define SYS_LSEEK   15
#define SYS_SIGNAL  16
#define SYS_KILL    17
#define SYS_SIGACTION 18
#define SYS_SIGRETURN 19
#define SYS_SIGPROCMASK 20
#define SYS_SLEEP   27
#define SYS_UPTIME  26
#define SYS_SBRK    30
#define SYS_MKDIR   45
#define SYS_READLINK 48
#define SYS_PIPE    54

/* Device session handles (driver syscalls) */
#define SYS_DEV_OPEN   84
#define SYS_DEV_CLOSE  85
#define SYS_DEV_INFO   86
#define SYS_IOCTL   21
#define SYS_GETUID  55
#define SYS_GETEUID 56
#define SYS_GETGID  57
#define SYS_GETEGID 58
#define SYS_SETUID  59
#define SYS_SETGID  60
#define SYS_CLOCK_GETTIME 70
#define SYS_SELECT  74
#define SYS_POLL    75
#define SYS_CLONE   76
#define SYS_FUTEX   77
#define SYS_MMAP    78
#define SYS_MUNMAP  79
#define SYS_MPROTECT 80
#define SYS_GETTID  29
#define SYS_GETDENTS 68
#define SYS_MOUNT   51
#define SYS_DUP2    53

/* POSIX additions */
#define SYS_FCNTL       87
#define SYS_LSTAT       88
#define SYS_PREAD       89
#define SYS_PWRITE      90
#define SYS_UNAME       91
#define SYS_SYSINFO     92
#define SYS_GETRLIMIT   93
#define SYS_SETRLIMIT   94

/* Networking (BSD sockets — lwIP backend in bsd/net) */
#define SYS_SOCKET      95
#define SYS_BIND        96
#define SYS_CONNECT     97
#define SYS_LISTEN      98
#define SYS_ACCEPT      99
#define SYS_GETSOCKNAME 100
#define SYS_GETPEERNAME 101
#define SYS_SENDTO      102
#define SYS_RECVFROM    103
#define SYS_SETSOCKOPT  104
#define SYS_GETSOCKOPT  105
#define SYS_SHUTDOWN    106

#define BSD_SYS(n) (1024L + (n))

#if defined(__x86_64__)
static long syscall7(long num, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    __asm__ volatile(
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "movq %7, %%r9\n\t"
        "syscall\n\t"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "rm"(a3), "rm"(a4), "rm"(a5)
        : "rcx", "r11", "r10", "r8", "r9", "memory");
    return ret;
}
#elif defined(__aarch64__)
static long syscall7(long num, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long x0 __asm__("x0") = num;
    register long x1 __asm__("x1") = a0;
    register long x2 __asm__("x2") = a1;
    register long x3 __asm__("x3") = a2;
    register long x4 __asm__("x4") = a3;
    register long x5 __asm__("x5") = a4;
    register long x6 __asm__("x6") = a5;
    __asm__ volatile(
        "svc #0\n\t"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6)
        : "memory");
    ret = x0;
    return ret;
}
#elif defined(__i386__)
static long syscall7(long num, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long ebp_val __asm__("ebp") = a5;   /* 6th arg travels in ebp */
    __asm__ volatile(
        "int $0x80\n\t"
        : "=a"(ret)
        : "a"(num), "b"(a0), "c"(a1), "d"(a2), "S"(a3), "D"(a4), "r"(ebp_val)
        : "memory");
    return ret;
}
#else
#error "Unsupported architecture"
#endif

static long syscall6(long num, long a0, long a1, long a2, long a3, long a4) {
    return syscall7(num, a0, a1, a2, a3, a4, 0);
}

static long syscall3(long num, long a0, long a1, long a2) {
    return syscall6(num, a0, a1, a2, 0, 0);
}
static long syscall2(long num, long a0, long a1) {
    return syscall3(num, a0, a1, 0);
}
static long syscall1(long num, long a0) {
    return syscall2(num, a0, 0);
}
static long syscall0(long num) {
    return syscall1(num, 0);
}

/* ---- mmap(2) family ---- */

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_FAILED     ((void *)-1)

static void *mmap(void *addr, size_t len, int prot, int flags,
                   int fd, long offset) {
    return (void *)syscall7(BSD_SYS(SYS_MMAP), (long)addr, (long)len,
                            prot, flags, fd, offset);
}
static long munmap(void *addr, size_t len) {
    return syscall2(BSD_SYS(SYS_MUNMAP), (long)addr, (long)len);
}
static int mprotect(void *addr, size_t len, int prot) {
    return (int)syscall3(BSD_SYS(SYS_MPROTECT), (long)addr, (long)len, prot);
}

/* ---- POSIX additions ---- */

#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define FD_CLOEXEC 1

#define O_APPEND   0x400
#define O_NONBLOCK 0x800

static int fcntl(int fd, int cmd, long arg) {
    return (int)syscall3(BSD_SYS(SYS_FCNTL), fd, cmd, arg);
}
static long lstat(const char *path, void *statbuf) {
    return syscall2(BSD_SYS(SYS_LSTAT), (long)path, (long)statbuf);
}
static long pread(int fd, void *buf, size_t count, long offset) {
    return syscall6(BSD_SYS(SYS_PREAD), fd, (long)buf, count, offset, 0);
}
static long pwrite(int fd, const void *buf, size_t count, long offset) {
    return syscall6(BSD_SYS(SYS_PWRITE), fd, (long)buf, count, offset, 0);
}
static long uname(void *buf) {
    return syscall1(BSD_SYS(SYS_UNAME), (long)buf);
}
static long sysinfo(void *buf) {
    return syscall1(BSD_SYS(SYS_SYSINFO), (long)buf);
}
static long getrlimit(int which, void *rlim) {
    return syscall2(BSD_SYS(SYS_GETRLIMIT), which, (long)rlim);
}
static long setrlimit(int which, void *rlim) {
    return syscall2(BSD_SYS(SYS_SETRLIMIT), which, (long)rlim);
}

/* ---- BSD sockets (kernel backend: bsd/net socketfs on lwIP) ---- */

/* Values match bsd/include/bsd/socket.h (wire ABI) */
#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define SOL_SOCKET      0xfff
#define SO_REUSEADDR    0x0004

#define POLLIN          0x001
#define POLLOUT         0x004
#define POLLERR         0x008

struct sockaddr {
    unsigned char sa_len;
    unsigned char sa_family;
    char          sa_data[14];
};

struct sockaddr_in {
    unsigned char  sin_len;    /* set to sizeof(struct sockaddr_in) */
    unsigned char  sin_family;
    unsigned short sin_port;   /* network byte order */
    unsigned int   sin_addr;   /* network byte order */
    char           sin_zero[8];
};

struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* Byte-swap helpers (all supported targets are little-endian). */
static unsigned short htons(unsigned short v) {
    return (unsigned short)((v << 8) | (v >> 8));
}
static unsigned short ntohs(unsigned short v) { return htons(v); }
static unsigned int htonl(unsigned int v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | (v >> 24);
}
static unsigned int ntohl(unsigned int v) { return htonl(v); }

static int socket(int domain, int type, int protocol) {
    return (int)syscall3(BSD_SYS(SYS_SOCKET), domain, type, protocol);
}

static int bind(int fd, const struct sockaddr *addr, unsigned int addrlen) {
    return (int)syscall3(BSD_SYS(SYS_BIND), fd, (long)addr, addrlen);
}

static int listen(int fd, int backlog) {
    return (int)syscall2(BSD_SYS(SYS_LISTEN), fd, backlog);
}

static int accept(int fd, struct sockaddr *addr, unsigned int *addrlen) {
    return (int)syscall3(BSD_SYS(SYS_ACCEPT), fd, (long)addr, (long)addrlen);
}

static int connect(int fd, const struct sockaddr *addr, unsigned int addrlen) {
    return (int)syscall3(BSD_SYS(SYS_CONNECT), fd, (long)addr, addrlen);
}

static long sendto(int fd, const void *buf, unsigned long len, int flags,
                   const struct sockaddr *dest_addr, unsigned int addrlen) {
    return syscall7(BSD_SYS(SYS_SENDTO), fd, (long)buf, len, flags,
                    (long)dest_addr, addrlen);
}

static long recvfrom(int fd, void *buf, unsigned long len, int flags,
                     struct sockaddr *src_addr, unsigned int *addrlen) {
    return syscall7(BSD_SYS(SYS_RECVFROM), fd, (long)buf, len, flags,
                    (long)src_addr, (long)addrlen);
}

static int getsockname(int fd, struct sockaddr *addr, unsigned int *addrlen) {
    return (int)syscall3(BSD_SYS(SYS_GETSOCKNAME), fd, (long)addr, (long)addrlen);
}

static int getpeername(int fd, struct sockaddr *addr, unsigned int *addrlen) {
    return (int)syscall3(BSD_SYS(SYS_GETPEERNAME), fd, (long)addr, (long)addrlen);
}

static int setsockopt(int fd, int level, int optname, const void *optval,
                      unsigned int optlen) {
    return (int)syscall6(BSD_SYS(SYS_SETSOCKOPT), fd, level, optname,
                         (long)optval, optlen);
}

static int shutdown(int fd, int how) {
    return (int)syscall2(BSD_SYS(SYS_SHUTDOWN), fd, how);
}

static int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    return (int)syscall3(BSD_SYS(SYS_POLL), (long)fds, nfds, timeout);
}

#endif
