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


#ifndef MEMORY_H
#define MEMORY_H

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

/* User address space layout.
 *
 * All user VAs live in the first 1GB (L0[0] -> L1[0], L2 indices 2..63),
 * which vmm_create_directory() gives to each process as PRIVATE tables.
 * Everything at or above 0x40000000 is the kernel's shared RAM window;
 * device MMIO (GIC, UART, virtio, ECAM) lives in shared L2 slots
 * 64..96 / 511 of L1[0] and is kernel-access-only.
 *
 * Historically these constants pointed into the identity RAM window,
 * which made every process alias the same physical pages — fork/exec
 * of a second process executed the first one's binary. */
#define USER_BASE          0x0000000000400000ULL
#define USER_HEAP_START    0x0000000000800000ULL
#define USER_MMAP_START    0x0000000000C00000ULL
#define USER_STACK_TOP     0x0000000003F00000ULL
#define USER_STACK_PAGES   32
#define USER_TLS_VADDR     0x0000000003A00000ULL

#endif
