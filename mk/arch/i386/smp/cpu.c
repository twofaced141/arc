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


/* i386 SMP stub: single-CPU kernel.  Keeps generic mk/smp code
 * linkable; real per-CPU support is amd64/arm64 only. */
#include "cpu.h"

int arch_cpu_discover(void) {
    cpus[0].id = 0;
    cpus[0].hw_id = 0;
    cpus[0].arch.apic_id = 0;
    return 1;
}

int arch_percpu_init(struct cpu *cpu) {
    cpu->arch.self = cpu;
    cpu->arch.ipi_pending = 0;
    cpu->arch.need_resched = 0;
    return 0;
}

int arch_cpu_start(struct cpu *cpu) {
    (void)cpu;
    return -1;
}

void arch_ap_entry(struct cpu *cpu) {
    cpu_ap_main(cpu);
}

void arch_cpu_mark_pending(struct cpu *cpu, unsigned type) {
    __atomic_or_fetch(&cpu->arch.ipi_pending, 1u << type, __ATOMIC_RELEASE);
}

void arch_cpu_send_ipi(struct cpu *cpu, unsigned type) {
    (void)cpu;
    (void)type;
}

void arch_cpu_idle(void) {
    __asm__ __volatile__("sti");
    __asm__ __volatile__("hlt");
}

void arch_cpu_stop_self(void) {
    for (;;) {
        __asm__ __volatile__("cli");
        __asm__ __volatile__("hlt");
    }
}
