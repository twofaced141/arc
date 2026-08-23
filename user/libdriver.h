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


/* libdriver.h — Userspace driver API
 *
 * Syscall ABI (amd64):
 *   rax = 1024 + syscall_num, args: rdi, rsi, rdx, r10, r8, r9
 *   syscall instruction, ret in rax, clobbers rcx, r11
 *
 * i386:
 *   eax = 1024 + syscall_num, args: ebx, ecx, edx, esi, edi
 *   int 0x80
 */

#ifndef LIBDRIVER_H
#define LIBDRIVER_H

#include <stdint.h>
#include <stddef.h>

#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_WAITPID     6
#define SYS_GETPID      7
#define SYS_EXECVE      11
#define SYS_LSEEK       15
#define SYS_SLEEP       27

#define SYS_PHYS_MAP        31
#define SYS_DMA_ALLOC       32
#define SYS_IRQ_SUBSCRIBE   33
#define SYS_IRQ_WAIT        34
#define SYS_PORT_IN         35
#define SYS_PORT_OUT        36
#define SYS_SERVICE_REGISTER 37
#define SYS_SERVICE_LOOKUP  38
#define SYS_SERVICE_QUERY   43
#define SYS_PCI_DEVICE_INFO 39
#define SYS_DEVICE_INFO     83
#define SYS_DEV_OPEN        84
#define SYS_DEV_CLOSE       85
#define SYS_DEV_INFO        86
#define SYS_IO_REGISTER     40
#define SYS_IO_GET_REQUEST  41
#define SYS_IO_COMPLETE     42

#define BSD_SYS(n) (1024L + (n))


typedef struct pci_dev {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  irq_line;
    uint64_t bar[6];
} pci_dev_t;

/* Generic device descriptor — must match kernel's arc_device_info_t
 * (mk/dev/include/device.h).  Bus-agnostic: works for PCI today and
 * any future bus (platform, USB, ...) registered in the kernel. */
#define DEV_INFO_BUS_MAX  16
#define DEV_INFO_NAME_MAX 48
#define DEV_INFO_DRV_MAX  32
#define DEV_MAX_RESOURCES 8

typedef struct {
    uint32_t type;
    uint64_t start;
    uint64_t size;
} dev_resource_t;

typedef struct {
    char     bus[DEV_INFO_BUS_MAX];
    char     name[DEV_INFO_NAME_MAX];
    uint32_t type;
    uint32_t state;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    char     driver[DEV_INFO_DRV_MAX];
    uint32_t resource_count;
    dev_resource_t resources[DEV_MAX_RESOURCES];
} device_info_t;

/* Device framework type/state enums (mirror mk/dev/include/device.h) */
#define DEV_TYPE_UNKNOWN 0xFF
#define DEV_TYPE_PCI     0x01
#define DEV_TYPE_PLATFORM 0x08

#define DEV_STATE_NEW        0
#define DEV_STATE_REGISTERED 1
#define DEV_STATE_PROBED     2
#define DEV_STATE_READY      3
#define DEV_STATE_FAILED     4

#define DEV_RES_MMIO 0
#define DEV_RES_IRQ  1
#define DEV_RES_DMA  2

typedef struct dma_buf {
    void     *virt;
    uint32_t  phys;
} dma_buf_t;

/* I/O request (must match kernel's io_request) */
#define IO_REQ_MAX_ARG  4

struct io_request {
    uint64_t request_id;
    uint32_t opcode;
    uint32_t flags;
    uint64_t arg[IO_REQ_MAX_ARG];
    uint64_t buf_phys;
    uint64_t buf_size;
};


#if defined(__x86_64__)
static inline long syscall6(long num, long a0, long a1, long a2,
                            long a3, long a4)
{
    long ret;
    __asm__ volatile(
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "syscall\n\t"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "rm"(a3), "rm"(a4)
        : "rcx", "r11", "r10", "r8", "memory");
    return ret;
}
#elif defined(__i386__)
static inline long syscall6(long num, long a0, long a1, long a2,
                            long a3, long a4)
{
    long ret;
    (void)a4;
    __asm__ volatile(
        "int $0x80\n\t"
        : "=a"(ret)
        : "a"(num), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
        : "memory");
    return ret;
}
#elif defined(__aarch64__)
static inline long syscall6(long num, long a0, long a1, long a2,
                            long a3, long a4)
{
    long ret;
    register long x0 __asm__("x0") = num;
    register long x1 __asm__("x1") = a0;
    register long x2 __asm__("x2") = a1;
    register long x3 __asm__("x3") = a2;
    register long x4 __asm__("x4") = a3;
    register long x5 __asm__("x5") = a4;
    __asm__ volatile(
        "svc #0\n\t"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "memory");
    ret = x0;
    return ret;
}
#else
#error "Unsupported architecture for libdriver"
#endif

/* Wrappers: syscallN(num, ...) sends N+1 total values (num + N data args) */
static inline long syscall5(long num, long a0, long a1, long a2, long a3) {
    return syscall6(num, a0, a1, a2, a3, 0);
}
static inline long syscall4(long num, long a0, long a1, long a2) {
    return syscall6(num, a0, a1, a2, 0, 0);
}
static inline long syscall3(long num, long a0, long a1) {
    return syscall6(num, a0, a1, 0, 0, 0);
}
static inline long syscall2(long num, long a0) {
    return syscall6(num, a0, 0, 0, 0, 0);
}
static inline long syscall1(long num) {
    return syscall6(num, 0, 0, 0, 0, 0);
}
/* syscall0 — just the syscall number, no data args */
static inline long syscall0(long num) {
    return syscall6(num, 0, 0, 0, 0, 0);
}


/* ---- Console ---- */
static inline long driver_write(int fd, const void *buf, unsigned long cnt) {
    return syscall4(BSD_SYS(SYS_WRITE), fd, (long)buf, cnt);
}
static inline long driver_exit(int code) {
    return syscall2(BSD_SYS(SYS_EXIT), code);
}

/* ---- Time ---- */
static inline long driver_sleep(long secs) {
    return syscall2(BSD_SYS(SYS_SLEEP), secs);
}

/* ---- MMIO: map physical memory into process address space ------------
 * Returns virtual address, or -1 on failure.
 * If virt_hint == 0, kernel auto-allocates.
 * Requires an open handle (dev_open) to a device whose MMIO resource
 * covers the requested range. */
static inline void *phys_map(uint64_t phys, size_t size, int cache_disable) {
    long ret = syscall5(BSD_SYS(SYS_PHYS_MAP), phys, 0, size, cache_disable);
    return (void *)ret;
}

/* ---- DMA: allocate physically contiguous buffer ----
 * Requires at least one open device handle. */
static inline int dma_alloc(size_t size, dma_buf_t *buf) {
    return (int)syscall4(BSD_SYS(SYS_DMA_ALLOC), size, (long)buf, 0);
}

/* ---- IRQ ----
 * irq_subscribe requires an open handle to a device whose IRQ resource
 * is the requested line.  Subscriptions are revoked automatically when
 * the process exits. */
static inline int irq_subscribe(int irq) {
    return (int)syscall2(BSD_SYS(SYS_IRQ_SUBSCRIBE), irq);
}
static inline int irq_wait(void) {
    return (int)syscall1(BSD_SYS(SYS_IRQ_WAIT));
}

/* ---- x86 I/O ports ----
 * Requires an open handle to a device whose resource covers the port
 * (PCI I/O-space BARs count as MMIO resources). */
static inline long port_in(uint16_t port, int size) {
    return syscall3(BSD_SYS(SYS_PORT_IN), port, size);
}
static inline long port_out(uint16_t port, uint32_t value, int size) {
    return syscall4(BSD_SYS(SYS_PORT_OUT), port, value, size);
}

/* ---- I/O Channels ---- */
static inline int io_create(const char *name) {
    return (int)syscall2(BSD_SYS(SYS_IO_REGISTER), (long)name);
}

/* Register a block-flavored channel: the kernel attaches a block
 * device (named after the channel without the "block/" prefix) whose
 * read/write are forwarded as io_requests.  num_blocks may exceed 32
 * bits; it travels as lo+hi halves. */
static inline int io_create_block(const char *name, uint32_t block_size,
                                  uint64_t num_blocks) {
    return (int)syscall5(BSD_SYS(SYS_IO_REGISTER), (long)name,
                         (long)block_size,
                         (long)(num_blocks & 0xFFFFFFFFu),
                         (long)(num_blocks >> 32));
}
static inline int io_get_request(int handle, struct io_request *req) {
    return (int)syscall3(BSD_SYS(SYS_IO_GET_REQUEST), handle, (long)req);
}
static inline int io_complete(int handle, uint64_t rid, int result) {
    /* 64-bit request_id travels as lo+hi halves: every ARGn is a
     * 32-bit register on i386, so full-width args must be split
     * (same convention as SYS_IO_REGISTER's num_blocks). */
    return (int)syscall5(BSD_SYS(SYS_IO_COMPLETE), handle,
                         (long)(rid & 0xFFFFFFFFu),
                         (long)(rid >> 32), result);
}

/* ---- Service registry ---- */
static inline int svc_register(const char *name, uint64_t data) {
    return (int)syscall3(BSD_SYS(SYS_SERVICE_REGISTER), (long)name, data);
}
static inline int svc_register_desc(const char *name, const char *desc,
                                    uint64_t data) {
    return (int)syscall4(BSD_SYS(SYS_SERVICE_REGISTER), (long)name, data,
                         (long)desc);
}
static inline long svc_lookup(const char *name) {
    return syscall2(BSD_SYS(SYS_SERVICE_LOOKUP), (long)name);
}
/* Copy a service's description string into buf (len bytes).  Returns the
 * number of bytes copied (including NUL), or -1 if the service is not
 * registered. */
static inline long svc_query(const char *name, char *buf, unsigned long len) {
    return syscall4(BSD_SYS(SYS_SERVICE_QUERY), (long)name, (long)buf, len);
}

/* ---- Devices (generic driver framework) ---- */
int dev_enum(int index, device_info_t *dev);

/* Opaque per-process device session handles.  A handle references the
 * device's stable identity (bus + name), not a position in the kernel's
 * device list: it stays valid while the device exists, and the kernel
 * drops every handle of a process when it exits.
 *
 *   int h = dev_open("pci", "pci0000:00:1f.2");   (0 = invalid)
 *   device_info_t info;
 *   dev_info(h, &info);                            (< 0 if gone)
 *   dev_close(h);
 */
int dev_open(const char *bus, const char *name);
int dev_close(int handle);
int dev_info(int handle, device_info_t *dev);

/* ---- PCI ---- */
int pci_find(uint8_t cls, uint8_t subcls, int idx, pci_dev_t *dev);

/* ---- Console helpers ---- */
void puts(const char *s);
void puthex(uint64_t v);
void putdec(int64_t v);

/* ---- Freestanding memory ops (implemented in libdriver.c) ---- */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

/* ---- Fork/exec helpers (for standalone drivers) ---- */
static inline long driver_fork(void) {
    return syscall1(BSD_SYS(SYS_FORK));
}
static inline long driver_execve(const char *path) {
    return syscall3(BSD_SYS(SYS_EXECVE), (long)path, 0);
}

#endif /* LIBDRIVER_H */
