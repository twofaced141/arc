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


#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>
#include <stddef.h>

struct cpu;

/* Per-CPU arch state (x86_64).  GS.base points at the start of this
 * structure during kernel execution, so cpu_current() is a single
 * `movq %gs:0` load (see arch_cpu_current below).
 *
 * NOTE: GS.base is only ever written from kernel mode (IA32_GS_BASE
 * MSR).  No swapgs is used anywhere in the kernel, and user TLS uses
 * FS on amd64 — so %gs stays kernel-private on all CPUs. */
struct arch_cpu {
    struct cpu *self;           /* offset 0: cpu_current() reads %gs:0 */
    uint32_t apic_id;           /* hw id (MADT Processor Local APIC ID) */
    uint32_t ipi_pending;       /* bitmask of pending IPI types */
    int      need_resched;      /* Phase 11: set by scheduler_ipi() */
    uint64_t stack_base;        /* per-CPU kernel stack (AP trampoline) */
    uint64_t syscall_rsp0;      /* offset 32: syscall_entry reads %gs:32 */
    uint64_t user_rsp0;         /* offset 40: saved user RSP (per-CPU!) */
};

/* interrupts.s loads %gs:32 for the syscall entry stack and stores the
 * interrupted user RSP at %gs:40 — keep both fields at fixed offsets
 * and refuse to build if they move.  Both MUST be per-CPU: a single
 * global let two CPUs entering syscalls concurrently corrupt each
 * other's saved frame. */
_Static_assert(offsetof(struct arch_cpu, syscall_rsp0) == 32,
               "interrupts.s expects syscall_rsp0 at offset 32");
_Static_assert(offsetof(struct arch_cpu, user_rsp0) == 40,
               "interrupts.s expects user_rsp0 at offset 40");

static inline struct cpu *arch_cpu_current(void) {
    struct cpu *self;
    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(self));
    return self;
}

static inline void arch_cpu_relax(void) {
    __asm__ __volatile__("pause");
}

/* Phase 8: mark a pending-IPI bit before sending the IPI. */
void arch_cpu_mark_pending(struct cpu *cpu, unsigned type);

/* Phase 1: fill cpus[] from ACPI MADT.  Returns count, 0 on failure. */
int arch_cpu_discover(void);

/* Phase 3: point GS.base at cpu->arch and prepare per-CPU state. */
int arch_percpu_init(struct cpu *cpu);

/* Phase 4: INIT/SIPI bringup of one AP. */
int arch_cpu_start(struct cpu *cpu);

/* Phase 8: architecture-specific IPI send. */
void arch_cpu_send_ipi(struct cpu *cpu, unsigned type);

/* Phase 6: idle loop on the AP. */
void arch_cpu_idle(void);

/* Phase 9: stop this CPU (IPI_STOP / cpu_stop). */
void arch_cpu_stop_self(void);

/* Phase 5: AP trampoline jumps here (long mode, own stack, GS set). */
void arch_ap_entry(struct cpu *cpu);

/* Install the architecture IPI delivery (IDT vector / GIC SGI). */
void ipi_init(void);

#endif
