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


#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>
#include "arch_cpu.h"
#include "thread.h"

#define CPU_MAX 64

/* CPU lifecycle states (Phase 7 handshake) */
enum cpu_state {
    CPU_OFFLINE,
    CPU_STARTING,
    CPU_ONLINE,
    CPU_FAILED,
};

/* Generic IPI types (Phase 8).  Architecture delivers the event;
 * the dispatch is done by ipi_handler(). */
enum ipi_type {
    IPI_RESCHEDULE = 0,
    IPI_CALL       = 1,
    IPI_TLB        = 2,
    IPI_STOP       = 3,
};

struct runqueue;   /* Phase 12: per-CPU runqueue (arch scheduler.h) */

/* Per-CPU state.  Generic fields only — anything arch-specific lives
 * in struct arch_cpu (arch_cpu.h). */
struct cpu {
    unsigned id;                /* kernel CPU id (0..cpu_count-1) */
    unsigned hw_id;             /* APIC ID / MPIDR (arch-neutral view) */
    volatile enum cpu_state state;

    struct thread *current;
    struct thread *idle;
    struct runqueue *runqueue;
    void *kernel_stack;

    volatile unsigned long ipi_received; /* Phase 11: monotonic IPI count */

    struct arch_cpu arch;
};

extern struct cpu cpus[CPU_MAX];
extern unsigned cpu_nr;         /* discovered CPU count */

/* Phase 3: per-CPU access.  On x86 via GS.base, on arm64 via TPIDR_EL1. */
static inline struct cpu *cpu_current(void) {
    return arch_cpu_current();
}

struct cpu *cpu_get(unsigned id);
unsigned cpu_count(void);
bool cpu_online(struct cpu *cpu);

/* BSP-side: discover CPUs (Phase 1) and bring them up (Phase 4). */
void cpu_init(void);
int  cpu_start(struct cpu *cpu);
void cpu_stop(struct cpu *cpu);

/* Start every discovered AP.  Returns the number that came online. */
int cpu_start_all(void);

/* AP entry (Phase 6) — runs on the AP after the trampoline. */
void cpu_ap_main(struct cpu *cpu);
void cpu_mark_online(struct cpu *cpu);

/* Per-CPU scheduler init (Phase 12).  Weak no-op until per-CPU
 * runqueues exist. */
void scheduler_init_cpu(struct cpu *cpu);

/* IPI (Phase 8-9). */
void cpu_send_ipi(struct cpu *cpu, enum ipi_type type);
void ipi_handler(enum ipi_type type);
void cpu_call_process(void);
void tlb_ipi(void);

/* Flush the TLB on every OTHER online CPU (full CR3 reload via
 * IPI_TLB).  Required after changing PTEs that may be cached in
 * remote TLBs — e.g. COW-marking a parent's writable pages in fork. */
void tlb_flush_others(void);

/* Cross-CPU call (Phase 13). */
void cpu_call(struct cpu *cpu, void (*fn)(void *), void *arg);

/* IPI_RESCHEDULE target (Phase 11). */
void scheduler_ipi(void);

#endif
