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
#include "cpu.h"

/* GDT: null, kernel code/data, user segments, then one 16-byte TSS
 * descriptor per CPU.  CPU 0 keeps the historic selector 0x30;
 * CPU i>0 gets 0x40 + 0x10*(i-1).  All CPUs share the same GDT image
 * (the AP trampoline lgdt's it), but each loads its own TSS via ltr. */
#define GDT_TSS0_ENTRY 6
#define GDT_TSS0_SEL   0x30
#define GDT_TSS_ENTRY(i)  (GDT_TSS0_ENTRY + 2 * (i))
#define GDT_TSS_SEL(i)    ((i) == 0 ? GDT_TSS0_SEL \
                                    : (uint16_t)(0x40 + 0x10 * ((i) - 1)))

static uint64_t gdt_entries[GDT_TSS0_ENTRY + 2 * CPU_MAX];
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr;

static struct tss per_cpu_tss[CPU_MAX];

/* Dedicated stacks for exceptions that must never run on the thread stack:
 * a double fault (e.g. overflowed/corrupt kernel stack) or NMI landing on
 * the broken stack would otherwise push it into a triple fault.  Shared by
 * all CPUs' TSS IST entries (a simultaneous DF/NMI on two CPUs is not
 * survivable anyway). */
#define DF_STACK_SIZE  16384
#define NMI_STACK_SIZE 8192
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t nmi_stack[NMI_STACK_SIZE] __attribute__((aligned(16)));

void tss_set_kernel_stack(uint64_t rsp0) {
    struct cpu *c = cpu_current();
    unsigned id = c ? c->id : 0;
    per_cpu_tss[id].rsp0 = rsp0;
    if (c)
        c->arch.syscall_rsp0 = rsp0;
}

static void gdt_set_tss_descriptor(int e, uint64_t base, uint32_t limit) {
    gdt_entries[e] =
        ((uint64_t)limit & 0xFFFF) |
        ((base & 0xFFFF) << 16) |
        (((base >> 16) & 0xFF) << 32) |
        (0x89ULL << 40) |
        ((((uint64_t)limit >> 16) & 0x0F) << 48) |
        ((base >> 24) & 0xFFULL) << 56;
    gdt_entries[e + 1] = (base >> 32);
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

    /* Clear all TSSes */
    for (unsigned i = 0; i < CPU_MAX; i++) {
        for (uint32_t w = 0; w < sizeof(struct tss) / 8; w++)
            ((uint64_t *)&per_cpu_tss[i])[w] = 0;
        per_cpu_tss[i].ist1 = (uint64_t)(uintptr_t)&df_stack[DF_STACK_SIZE];
        per_cpu_tss[i].ist2 = (uint64_t)(uintptr_t)&nmi_stack[NMI_STACK_SIZE];
        gdt_set_tss_descriptor(GDT_TSS_ENTRY(i), (uint64_t)&per_cpu_tss[i],
                               sizeof(struct tss) - 1);
    }

    {
        uint64_t rsp;
        __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
        per_cpu_tss[0].rsp0 = rsp;
        cpus[0].arch.syscall_rsp0 = rsp;
    }

    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base = (uint64_t)&gdt_entries[0];

    __asm__ __volatile__("lgdt %0" : : "m"(gdt_ptr));
    /* NB: GS is deliberately NOT reloaded here.  Loading a data
     * selector into %gs replaces its hidden base with the descriptor
     * base (0), silently destroying the per-CPU IA32_GS_BASE written
     * by arch_percpu_init().  Any code reading %gs:offset between this
     * reload and the next arch_percpu_init() would dereference
     * address 0.  In long mode the selector value of GS is irrelevant;
     * only the MSR-set base matters. */
    __asm__ __volatile__(
        "push $0x08\n"
        "push $1f\n"
        "retfq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%ss\n"
        :
        :
        : "memory"
    );

    /* Load the BSP TSS (selector 0x30) */
    __asm__ __volatile__("ltr %0" : : "r"((uint16_t)GDT_TSS0_SEL));
}

/* AP-side: point TR at this CPU's TSS and seed rsp0/syscall_rsp0 with
 * the trampoline stack so a user-mode interrupt on the AP works even
 * before the first context switch.  Must run with GS set (cpu_current
 * valid) — arch_ap_entry guarantees this. */
void gdt_percpu_init(struct cpu *cpu) {
    unsigned id = cpu->id;
    uint64_t top = cpu->kernel_stack
        ? (uint64_t)(uintptr_t)cpu->kernel_stack + THREAD_KSTACK_SIZE : 0;
    per_cpu_tss[id].rsp0 = top;
    cpu->arch.syscall_rsp0 = top;
    __asm__ __volatile__("ltr %0" : : "r"(GDT_TSS_SEL(id)));
}
