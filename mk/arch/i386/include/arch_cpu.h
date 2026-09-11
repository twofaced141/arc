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

struct cpu;

/* Per-CPU arch state (i386).  UP-only: a single CPU, no AP bringup.
 * cpu_current() always returns &cpus[0] — the old %fs:0 load read
 * garbage because FS.base is never programmed on i386. */
struct arch_cpu {
    struct cpu *self;
    uint32_t apic_id;
    uint32_t ipi_pending;
    int      need_resched;
    uint64_t stack_base;
};

struct cpu *arch_cpu_current(void);

static inline void arch_cpu_relax(void) {
    __asm__ __volatile__("pause");
}

/* Phase 8: mark a pending-IPI bit before sending the IPI. */
void arch_cpu_mark_pending(struct cpu *cpu, unsigned type);

int arch_cpu_discover(void);
int arch_percpu_init(struct cpu *cpu);
int arch_cpu_start(struct cpu *cpu);
void arch_cpu_send_ipi(struct cpu *cpu, unsigned type);
void arch_cpu_idle(void);
void arch_cpu_stop_self(void);
void arch_ap_entry(struct cpu *cpu);

/* Install the architecture IPI delivery (IDT vector / GIC SGI). */
void ipi_init(void);

#endif
