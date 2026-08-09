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


#include "gdt.h"

/* GDT entries: 64-bit kernel code/data, 32-bit user segments, 64-bit user code,
 * TSS descriptor (2 entries wide) */
static uint64_t gdt_entries[8];
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr;

/* Non-static so syscall_entry (interrupts.s) can load rsp0 from it. */
struct tss kernel_tss;

/* Dedicated stacks for exceptions that must never run on the thread stack:
 * a double fault (e.g. overflowed/corrupt kernel stack) or NMI landing on
 * the broken stack would otherwise push it into a triple fault. */
#define DF_STACK_SIZE  16384
#define NMI_STACK_SIZE 8192
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t nmi_stack[NMI_STACK_SIZE] __attribute__((aligned(16)));

void tss_set_kernel_stack(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

/* Expose the installed GDT to the SMP code (AP trampoline shares it). */
void gdt_get_ptr(uint64_t *base, uint16_t *limit) {
    *base  = gdt_ptr.base;
    *limit = gdt_ptr.limit;
}

void gdt_install(void) {
    gdt_entries[0] = 0;                          /* Null descriptor */
    gdt_entries[1] = 0x0020980000000000ULL;      /* 64-bit kernel code  (sel=0x08) */
    gdt_entries[2] = 0x0000920000000000ULL;      /* 64-bit kernel data  (sel=0x10) */
    gdt_entries[3] = 0x00CFFA000000FFFFULL;      /* 32-bit user code    (sel=0x1B) DPL=3 */
    gdt_entries[4] = 0x00CFF2000000FFFFULL;      /* 32-bit user data    (sel=0x23) DPL=3 */
    gdt_entries[5] = 0x0020FA0000000000ULL;      /* 64-bit user code    (sel=0x2B) DPL=3 */

    /* Clear TSS */
    for (uint32_t i = 0; i < sizeof(struct tss) / 8; i++)
        ((uint64_t *)&kernel_tss)[i] = 0;

    /* Set initial kernel stack (will be updated per-thread by scheduler) */
    {
        uint64_t rsp;
        __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
        kernel_tss.rsp0 = rsp;
    }

    /* IST1 = double fault, IST2 = NMI: exceptions delivered through these
     * IST indices switch to a dedicated stack regardless of the (possibly
     * broken) interrupted stack, so the panic dump itself is safe. */
    kernel_tss.ist1 = (uint64_t)(uintptr_t)&df_stack[DF_STACK_SIZE];
    kernel_tss.ist2 = (uint64_t)(uintptr_t)&nmi_stack[NMI_STACK_SIZE];

    /* Build 64-bit TSS descriptor (2 qwords, sel=0x30) */
    {
        uint64_t base = (uint64_t)&kernel_tss;
        uint32_t limit = sizeof(struct tss) - 1;

        gdt_entries[6] =
            ((uint64_t)limit & 0xFFFF) |
            ((base & 0xFFFF) << 16) |
            (((base >> 16) & 0xFF) << 32) |
            (0x89ULL << 40) |
            ((((uint64_t)limit >> 16) & 0x0F) << 48) |
            ((base >> 24) & 0xFFULL) << 56;

        gdt_entries[7] = (base >> 32);
    }

    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base = (uint64_t)&gdt_entries[0];

    __asm__ __volatile__("lgdt %0" : : "m"(gdt_ptr));
    __asm__ __volatile__(
        "push $0x08\n"
        "push $1f\n"
        "retfq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        :
        :
        : "memory"
    );

    /* Load TSS (selector 0x30 = entry 6, TI=GDT, RPL=0) */
    __asm__ __volatile__("ltr %0" : : "r"((uint16_t)0x30));
}
