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


#include <stdint.h>
#include "memory.h"
#include "pmm.h"

extern uint64_t _kernel_end;

#define TOTAL_MEMORY (64 * 1024 * 1024ULL)
#define KERNEL_PHYS_BASE 0x40000000ULL

static uint64_t next_free;
static int ready;

void pmm_init(void) {
    uint64_t kend = (uint64_t)&_kernel_end;
    next_free = (kend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    ready = 1;
}

void *pmm_alloc_pages(uint32_t count) {
    if (!ready) return 0;
    uint64_t addr = next_free;
    uint64_t end = addr + count * PAGE_SIZE;
    if (end > KERNEL_PHYS_BASE + TOTAL_MEMORY) return 0;
    next_free = end;
    return (void *)addr;
}

void *pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_page(void *page) {
    pmm_free_pages(page, 1);
}

void pmm_free_pages(void *addr, uint32_t count) {
    (void)addr;
    (void)count;
}

uint32_t pmm_get_free_pages(void) {
    if (!ready) return 0;
    uint64_t free = (KERNEL_PHYS_BASE + TOTAL_MEMORY) - next_free;
    return (uint32_t)(free / PAGE_SIZE);
}

uint32_t pmm_get_total_pages(void) {
    return (uint32_t)(TOTAL_MEMORY / PAGE_SIZE);
}
