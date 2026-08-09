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


/* libdriver.c — Userspace driver library implementation
 *
 * Compiled with: gcc -ffreestanding -nostdlib -nostartfiles ... -c
 * Linked with driver ELF via driver.ld.
 */

#include "libdriver.h"


int pci_find(uint8_t cls, uint8_t subcls, int idx, pci_dev_t *dev)
{
    return (int)syscall5(BSD_SYS(SYS_PCI_DEVICE_INFO),
                         cls, subcls, idx, (long)dev);
}


int dev_enum(int index, device_info_t *dev)
{
    return (int)syscall3(BSD_SYS(SYS_DEVICE_INFO), index, (long)dev);
}

int dev_open(const char *bus, const char *name)
{
    return (int)syscall3(BSD_SYS(SYS_DEV_OPEN), (long)bus, (long)name);
}

int dev_close(int handle)
{
    return (int)syscall2(BSD_SYS(SYS_DEV_CLOSE), handle);
}

int dev_info(int handle, device_info_t *dev)
{
    return (int)syscall3(BSD_SYS(SYS_DEV_INFO), handle, (long)dev);
}


void puts(const char *s)
{
    unsigned long len = 0;
    while (s[len]) len++;
    driver_write(1, s, len);
}

void puthex(uint64_t v)
{
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) {
        unsigned d = (unsigned)(v & 0xF);
        buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
        v >>= 4;
    }
    driver_write(1, buf, 18);
}

void putdec(int64_t v)
{
    char buf[21];
    int i = 20;
    buf[20] = '\n';
    if (v < 0) {
        driver_write(1, "-", 1);
        v = -v;
    }
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);
    driver_write(1, buf + i, 21 - i);
}


void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return dst;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ((unsigned char *)dest)[i] = ((const unsigned char *)src)[i];
    return dest;
}
