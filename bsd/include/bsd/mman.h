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


#ifndef BSD_MMAN_H
#define BSD_MMAN_H

#include <stdint.h>
#include <stddef.h>
#include "isr.h"

/* mmap(2) protection bits */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* mmap(2) flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)(uintptr_t)-1)

/* Max mmap regions tracked per process */
#define MMAP_MAX_REGIONS 32

/* VMA tracking for mmap/munmap/mprotect (bsd/sys/sys_mmap.c).
 * vnode is opaque here (declared in bsd/vfs.h); a reference is held
 * for the lifetime of a file-backed region. */
typedef struct mmap_region {
    uintptr_t start;      /* page-aligned virtual address */
    size_t    len;        /* page-aligned length in bytes */
    int       prot;       /* PROT_* */
    int       flags;      /* MAP_PRIVATE / MAP_SHARED / MAP_FIXED */
    void     *vnode;      /* file backing (reference held), NULL if anon */
    int64_t   offset;     /* page-aligned file offset */
    uint8_t   used;
} mmap_region_t;

/* Forward decl — full definition in bsd/proc.h */
typedef struct proc proc_t;

/* Region management (called from bsd layers) */
void mmap_init(void);
mmap_region_t *mmap_region_find(proc_t *p, uintptr_t addr);
void mmap_teardown(proc_t *p);
void mmap_fork(proc_t *parent, proc_t *child);

/* Syscall handlers */
int64_t sys_mmap(proc_t *p, registers_t *r);
int64_t sys_munmap(proc_t *p, registers_t *r);
int64_t sys_mprotect(proc_t *p, registers_t *r);

#endif
