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

/* CPU lifecycle states (Phase 7 handshake + hotplug Phase 17) */
enum cpu_state {
    CPU_OFFLINE,
    CPU_STARTING,
    CPU_ONLINE,
    CPU_FAILED,
    CPU_STOPPING,    /* IPI_STOP in flight, CPU still scheduled */
    CPU_SUSPENDED,   /* offlined via cpu_offline(), stack/rq kept */
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

/* Phase 16: topology.  Decoded from APIC ID (x86) or MPIDR (ARM64);
 * scheduler uses package/core to prefer within-package stealing. */
struct cpu_topology {
    unsigned package;   /* socket / cluster (MPIDR Aff2 / APIC package) */
    unsigned core;      /* core within package (MPIDR Aff1 / APIC core) */
    unsigned thread;    /* SMT thread within core (MPIDR Aff0 low / APIC SMT) */
    unsigned smt;       /* 1 if this CPU shares a core with another CPU */
};

/* Per-CPU state.  Generic fields only — anything arch-specific lives
 * in struct arch_cpu (arch_cpu.h). */
struct cpu {
    unsigned id;                /* kernel CPU id (0..cpu_count-1) */
    uint64_t hw_id;             /* full APIC ID / MPIDR (never truncated) */
    volatile enum cpu_state state;

    struct thread *current;
    struct thread *idle;
    struct runqueue *runqueue;
    void *kernel_stack;

    volatile unsigned long ipi_received; /* Phase 11: monotonic IPI count */
    struct cpu_topology topo;   /* Phase 16: decoded topology */

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
void cpu_broadcast_ipi(enum ipi_type type);
void cpu_wakeup(struct cpu *cpu);
void ipi_handler(enum ipi_type type);
void cpu_call_process(void);
void tlb_ipi(void);

/* Online helpers. */
unsigned cpu_online_count(void);
uint64_t cpu_online_mask(void);

/* Hotplug (Phase 17): offline keeps stack/rq for later cpu_start;
 * BSP (id 0) can never be offlined.  Returns 0 on success. */
int cpu_offline(struct cpu *cpu);
int cpu_online_cpu(struct cpu *cpu);

/* Topology (Phase 16): decode helpers shared by all arches. */
void cpu_topo_decode_apic(struct cpu *cpu, uint32_t apic_id);
void cpu_topo_decode_mpidr(struct cpu *cpu, uint64_t mpidr);
int cpu_share_core(const struct cpu *a, const struct cpu *b);
int cpu_share_package(const struct cpu *a, const struct cpu *b);

/* Flush the TLB on every OTHER online CPU (full CR3 reload via
 * IPI_TLB).  Required after changing PTEs that may be cached in
 * remote TLBs — e.g. COW-marking a parent's writable pages in fork. */
void tlb_flush_others(void);
/* Synchronous variant: returns only after every other online CPU has
 * reloaded its TLB (ack via cpu_call_sync).  Process context, no locks
 * held — see the implementation for the exact contract. */
void tlb_flush_others_sync(void);

/* Cross-CPU call (Phase 13). */
void cpu_call(struct cpu *cpu, void (*fn)(void *), void *arg);
/* Synchronous variant: runs fn(arg) on target, spins until done.
 * done/arg must stay valid until return.  No-op if target==self. */
void cpu_call_sync(struct cpu *cpu, void (*fn)(void *), void *arg);
/* SMP stats for tests / shell. */
void cpu_dump_stats(void);
/* Boot selftest: UP checks always, IPI/cpu_call checks on SMP.
 * Prints "smp: selftest PASS/FAIL (...)".  Returns 0 on PASS. */
int smp_selftest(void);

/* IPI_RESCHEDULE target (Phase 11). */
void scheduler_ipi(void);

#endif
