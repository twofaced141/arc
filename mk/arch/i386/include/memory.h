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

#define PAGE_SIZE          4096
#define PAGE_SHIFT         12

/* Physical memory layout */
#define KERNEL_PHYS        0x100000

#ifdef __x86_64__
#define KERNEL_BASE        0xFFFFFFFF80000000ULL
#define DIRECT_MAP_SIZE    0x4000000ULL  /* 64MB identity via 2MB huge pages */

#define HEAP_START         0xFFFFFFFF90000000ULL
#define HEAP_END           0xFFFFFFFFA0000000ULL
#define HEAP_INITIAL_PAGES 16

#define TEMP_VADDR         0xFFFFFFFFFFFFF000ULL
#else
#define KERNEL_BASE        0xC0000000

#define HEAP_START         0xD0000000
#define HEAP_END           0xE0000000
#define HEAP_INITIAL_PAGES 4

#define TEMP_VADDR         0xFFC00000
#endif

/* User space layout (i386 defaults; amd64 has its own via mk/arch/amd64/include/memory.h) */
#ifndef __x86_64__
#define USER_BASE          0x08000000
#define USER_HEAP_START    0x40000000
#define USER_MMAP_START    0x50000000
#define USER_STACK_TOP     0xC0000000
#define USER_STACK_PAGES   32
#define USER_TLS_VADDR     0xBFFFB000
#endif

#endif
