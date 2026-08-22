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


/* AArch64 SMP: CPU discovery (Device Tree /cpus), per-CPU access via
 * TPIDR_EL1, GIC SGI IPI backend.  Secondary bringup via PSCI CPU_ON. */
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"
#include "fdt.h"
#include "gic.h"
#include "platform.h"
#include "psci.h"
#include "pmm.h"
#include "vmm.h"
#include "memory.h"
#include "isr.h"
#include "uart.h"
#include "thread.h"
#include "clkevt_arm.h"

/* DTB pointer saved by startup.s before BSS clear. */
extern uint64_t boot_dtb_ptr;

/* GIC v2 SGI: ID 16 is our generic IPI (16-31 are free SGIs). */
#define IPI_SGI_ID   16
#define GICD_SGIR    (*(volatile uint32_t *)(uintptr_t)(GICD_BASE + 0xF00))
#define GICD_SGIR_NSATT   (1 << 15)
#define GICD_SGIR_TARGET(x)  (((x) & 0xFF) << 16)

/* ------------------------------------------------------------------ */
/* Phase 1: discovery from Device Tree                                 */
/* ------------------------------------------------------------------ */

int arch_cpu_discover(void) {
    const void *dtb = (const void *)(uintptr_t)boot_dtb_ptr;
    if (!dtb)
        dtb = NULL;

    uint64_t mpidrs[CPU_MAX];
    int n = dtb ? fdt_get_cpus(dtb, mpidrs, CPU_MAX) : 0;

    if (n <= 0) {
        /* No DTB / no /cpus: single-CPU fallback with the real MPIDR. */
        uint64_t mpidr;
        __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
        cpus[0].id = 0;
        cpus[0].hw_id = (unsigned)(mpidr & 0xFF);
        cpus[0].arch.mpidr = mpidr;
        uart_print("smp: no DTB cpus, fallback 1 cpu\n");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        cpus[i].id = (unsigned)i;
        cpus[i].hw_id = (unsigned)(mpidrs[i] & 0xFF);
        cpus[i].arch.mpidr = mpidrs[i];
        uart_print("smp: cpu ");
        uart_print_hex64(i);
        uart_print(" -> mpidr ");
        uart_print_hex64(mpidrs[i]);
        uart_print("\n");
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Phase 3: per-CPU access via TPIDR_EL1                               */
/* ------------------------------------------------------------------ */

int arch_percpu_init(struct cpu *cpu) {
    cpu->arch.self = cpu;
    cpu->arch.ipi_pending = 0;
    cpu->arch.need_resched = 0;
    __asm__ __volatile__("msr tpidr_el1, %0" : : "r"(cpu) : "memory");
    return 0;
}

/* AP globals consumed by arch_secondary_entry (MMU off, identity-mapped) */
extern volatile uint64_t ap_stack;
extern volatile struct cpu *ap_cpu;
extern volatile uint64_t ap_ttbr;
extern void arch_secondary_entry(void);

/* ------------------------------------------------------------------ */
/* Phase 4: PSCI CPU_ON                                                */
/* ------------------------------------------------------------------ */

int arch_cpu_start(struct cpu *cpu) {
    if (!cpu)
        return -1;

    /* Allocate per-CPU kernel stack (identity-mapped) */
    void *stack = pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!stack) {
        uart_print("smp: no stack for cpu ");
        uart_print_hex64(cpu->id);
        uart_print("\n");
        return -1;
    }
    uint64_t stack_top = (uint64_t)(uintptr_t)stack + THREAD_KSTACK_SIZE;
    cpu->kernel_stack = stack;
    cpu->arch.stack_base = stack_top;

    /* Publish for secondary entry (MMU off) */
    ap_stack = stack_top;
    ap_cpu = cpu;
    page_directory_t *kdir = vmm_get_kernel_directory();
    if (!kdir) {
        uart_print("smp: no kernel page tables\n");
        return -1;
    }
    ap_ttbr = (uint64_t)(uintptr_t)kdir;
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");

    uint64_t entry = (uint64_t)(uintptr_t)arch_secondary_entry;
    uart_print("smp: PSCI CPU_ON cpu ");
    uart_print_hex64(cpu->id);
    uart_print(" mpidr ");
    uart_print_hex64(cpu->arch.mpidr);
    uart_print(" entry ");
    uart_print_hex64(entry);
    uart_print("\n");

    int ret = psci_cpu_on(cpu->arch.mpidr, entry, (uint64_t)(uintptr_t)cpu);
    if (ret != 0) {
        uart_print("smp: PSCI CPU_ON failed: ");
        uart_print_hex64((uint64_t)(uint32_t)ret);
        uart_print("\n");
        return -1;
    }
    return 0;
}

void arch_ap_entry(struct cpu *cpu) {
    gic_cpu_init();
    clkevt_arm_cpu_init();
    cpu_ap_main(cpu);
}

/* ------------------------------------------------------------------ */
/* Phase 8 + 9: GIC SGI backend                                        */
/* ------------------------------------------------------------------ */

/* Mark a pending-IPI bit.  GCC would route __atomic_or_fetch through
 * libatomic (__aarch64_ldset4_rel) at -O0 without LSE — inline it. */
void arch_cpu_mark_pending(struct cpu *cpu, unsigned type) {
    volatile uint32_t *p = &cpu->arch.ipi_pending;
    uint32_t mask = 1u << type;
    uint32_t tmp, status;
    __asm__ __volatile__(
        "1: ldxr %w0, [%2]\n"
        "   orr %w0, %w0, %w3\n"
        "   stlxr %w1, %w0, [%2]\n"
        "   cbnz %w1, 1b\n"
        : "=&r"(tmp), "=&r"(status)
        : "r"(p), "r"(mask)
        : "memory");
}

void arch_cpu_send_ipi(struct cpu *cpu, unsigned type) {
    (void)type;
    GICD_SGIR = GICD_SGIR_NSATT | GICD_SGIR_TARGET(cpu->hw_id) | IPI_SGI_ID;
}

/* Acquire-exchange of the pending-IPI mask.  GCC emits a libatomic
 * call (__aarch64_swp4_acq) at -O0 on aarch64 without LSE — do it
 * inline with ldxr/stlxr instead. */
static uint32_t sgi_exchange_pending(volatile uint32_t *p) {
    uint32_t pending;
    __asm__ __volatile__(
        "1: ldxr %w0, [%1]\n"
        "   stlxr w2, wzr, [%1]\n"
        "   cbnz w2, 1b\n"
        : "=&r"(pending)
        : "r"(p)
        : "memory", "w2");
    return pending;
}

/* IRQ handler for SGI: collect pending bits and dispatch generically.
 * EOI is done by the common irq_handler. */
static void ipi_irq_handler(registers_t *r) {
    (void)r;
    struct cpu *me = cpu_current();
    if (!me)
        return;

    uint32_t pending = sgi_exchange_pending(&me->arch.ipi_pending);
    unsigned type = 0;
    while (pending) {
        if (pending & 1)
            ipi_handler((enum ipi_type)type);
        pending >>= 1;
        type++;
    }
}

void ipi_init(void) {
    register_interrupt_handler(IPI_SGI_ID, ipi_irq_handler);
    uart_print("smp: IPI SGI 16 installed\n");
}

/* ------------------------------------------------------------------ */
/* Idle / stop                                                         */
/* ------------------------------------------------------------------ */

void arch_cpu_idle(void) {
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
    __asm__ __volatile__("wfi");
}

void arch_cpu_stop_self(void) {
    for (;;) {
        __asm__ __volatile__("msr daifset, #2" ::: "memory");
        __asm__ __volatile__("wfi");
    }
}
