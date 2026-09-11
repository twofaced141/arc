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


/* Generic SMP core: CPU lifecycle, IPI dispatch, cross-CPU calls.
 * Everything arch-specific is behind arch_cpu_* (arch_cpu.h). */
#include "cpu.h"
#include "spinlock.h"
#include "debug.h"
#include "panic.h"
#include <stddef.h>

struct cpu cpus[CPU_MAX];
unsigned cpu_nr;

/* ------------------------------------------------------------------ */
/* CPU lifecycle                                                       */
/* ------------------------------------------------------------------ */

struct cpu *cpu_get(unsigned id) {
    return (id < cpu_nr) ? &cpus[id] : NULL;
}

unsigned cpu_count(void) {
    return cpu_nr;
}

bool cpu_online(struct cpu *cpu) {
    return cpu && cpu->state == CPU_ONLINE;
}

/* Phase 1 + 3: discover CPUs from the platform (MADT / DT) and bring
 * the boot CPU online with its per-CPU state active. */
void cpu_init(void) {
    for (unsigned i = 0; i < CPU_MAX; i++) {
        cpus[i].id = i;
        cpus[i].state = CPU_OFFLINE;
        cpus[i].hw_id = 0;
        cpus[i].topo.package = 0;
        cpus[i].topo.core = i;
        cpus[i].topo.thread = 0;
        cpus[i].topo.smt = 0;
        cpus[i].arch.self = &cpus[i];
    }

    cpu_nr = (unsigned)arch_cpu_discover();
    if (cpu_nr == 0)
        cpu_nr = 1;   /* fallback: always at least the BSP */
    if (cpu_nr > CPU_MAX)
        cpu_nr = CPU_MAX;

    /* BSP is alive by definition. */
    cpus[0].state = CPU_ONLINE;
    arch_percpu_init(&cpus[0]);

    log_printf(LOG_LEVEL_INFO, "smp: %u CPU(s) found, boot cpu %u online (hw_id=%llu)\r\n",
               cpu_nr, cpus[0].id, (unsigned long long)cpus[0].hw_id);
}

/* Phase 4 + 7: start one AP.  OFFLINE/SUSPENDED/FAILED -> STARTING -> ONLINE.
 * Handshake spins with arch relax; ~5s budget at ~400M relax/s.  A FAILED
 * AP may be retried (state FAILED is accepted as a startable state). */
int cpu_start(struct cpu *cpu) {
    if (!cpu)
        return -1;
    if (cpu->state != CPU_OFFLINE && cpu->state != CPU_FAILED &&
        cpu->state != CPU_SUSPENDED)
        return -1;
    if (cpu->id == 0)
        return -1;  /* never "start" the BSP */

    log_printf(LOG_LEVEL_DEBUG, "smp: starting cpu %u (hw_id=%llu)\r\n",
               cpu->id, (unsigned long long)cpu->hw_id);
    cpu->state = CPU_STARTING;

    if (arch_cpu_start(cpu) < 0) {
        log_printf(LOG_LEVEL_ERROR, "smp: cpu %u start failed\r\n", cpu->id);
        cpu->state = CPU_FAILED;
        return -1;
    }

    /* Handshake: wait until the AP marks itself CPU_ONLINE. */
    uint64_t spins = 0;
    while (cpu->state != CPU_ONLINE) {
        arch_cpu_relax();
        if (++spins > 2000000000ULL) {
            log_printf(LOG_LEVEL_ERROR, "smp: cpu %u did not come online (state=%d)\r\n",
                       cpu->id, (int)cpu->state);
            cpu->state = CPU_FAILED;
            return -1;
        }
        /* AP may report FAILED itself (bad stack/GDT); stop waiting. */
        if (cpu->state == CPU_FAILED)
            return -1;
    }
    return 0;
}

void cpu_stop(struct cpu *cpu) {
    if (!cpu || cpu->state != CPU_ONLINE)
        return;
    if (cpu->id == 0)
        return;  /* BSP cannot be stopped */
    cpu->state = CPU_STOPPING;
    cpu_send_ipi(cpu, IPI_STOP);
}

/* Phase 17: hotplug.  Offline = STOPPING -> OFFLINE via IPI_STOP, but the
 * per-CPU stack/runqueue are kept (state SUSPENDED) so cpu_start() can
 * reuse them without realloc.  BSP can never be offlined. */
int cpu_offline(struct cpu *cpu) {
    if (!cpu || cpu->id == 0)
        return -1;
    if (cpu->state != CPU_ONLINE)
        return -1;
    cpu->state = CPU_STOPPING;
    cpu_send_ipi(cpu, IPI_STOP);
    /* Wait for the target to park itself (bounded spin). */
    uint64_t spins = 0;
    while (cpu->state == CPU_STOPPING) {
        arch_cpu_relax();
        if (++spins > 1000000000ULL)
            return -1;
    }
    /* cpu_stop_self() parks as OFFLINE; promote to SUSPENDED so the
     * stack/rq survive for a later cpu_start(). */
    if (cpu->state == CPU_OFFLINE)
        cpu->state = CPU_SUSPENDED;
    return (cpu->state == CPU_SUSPENDED) ? 0 : -1;
}

int cpu_online_cpu(struct cpu *cpu) {
    if (!cpu)
        return -1;
    if (cpu->state == CPU_ONLINE)
        return 0;
    return cpu_start(cpu);
}

unsigned cpu_online_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < cpu_nr; i++)
        if (cpus[i].state == CPU_ONLINE)
            n++;
    return n;
}

uint64_t cpu_online_mask(void) {
    uint64_t m = 0;
    for (unsigned i = 0; i < cpu_nr && i < 64; i++)
        if (cpus[i].state == CPU_ONLINE)
            m |= (1ULL << i);
    return m;
}

void cpu_dump_stats(void) {
    for (unsigned i = 0; i < cpu_nr; i++) {
        log_printf(LOG_LEVEL_INFO,
                   "smp: cpu%u hw=%llu state=%d ipi=%lu topo %u/%u/%u%s\r\n",
                   cpus[i].id, (unsigned long long)cpus[i].hw_id,
                   (int)cpus[i].state, cpus[i].ipi_received,
                   cpus[i].topo.package, cpus[i].topo.core,
                   cpus[i].topo.thread, cpus[i].topo.smt ? " smt" : "");
    }
}

static volatile int smp_ping_counter;

static void smp_ping_inc(void) {
#if defined(__aarch64__)
    /* No libatomic on freestanding aarch64 (-O0 w/o LSE lowers
     * __atomic_* to helpers); inline the add. */
    int tmp, status;
    __asm__ __volatile__(
        "1: ldxr %w0, [%2]\n"
        "   add %w0, %w0, #1\n"
        "   stlxr %w1, %w0, [%2]\n"
        "   cbnz %w1, 1b\n"
        : "=&r"(tmp), "=&r"(status)
        : "r"(&smp_ping_counter)
        : "memory");
#else
    __atomic_fetch_add(&smp_ping_counter, 1, __ATOMIC_RELAXED);
#endif
}

static void smp_ping_fn(void *arg) {
    (void)arg;
    smp_ping_inc();
}

/* Boot selftest.  Must be called on the BSP with interrupts enabled
 * (IPI delivery needs them).  UP: checks identity/topology/mask.
 * SMP: additionally pings every AP via cpu_call_sync and checks the
 * APs' monotonic IPI counters advance after a broadcast. */
int smp_selftest(void) {
    int fails = 0;
#define SMP_CHECK(name, cond) do { \
    if (!(cond)) { \
        log_printf(LOG_LEVEL_ERROR, "smp: selftest FAIL: %s\r\n", name); \
        fails++; \
    } \
} while (0)

    struct cpu *me = cpu_current();
    SMP_CHECK("current==cpus[0]", me == &cpus[0]);
    SMP_CHECK("count>=1", cpu_count() >= 1);
    SMP_CHECK("count<=CPU_MAX", cpu_count() <= (unsigned)CPU_MAX);
    SMP_CHECK("bsp online", cpu_online(&cpus[0]));
    SMP_CHECK("online_count>=1", cpu_online_count() >= 1);
    SMP_CHECK("online_mask bit0", (cpu_online_mask() & 1ULL) != 0);
    SMP_CHECK("hw_id topo", 1); /* decode ran in discover; dump shows it */

    if (cpu_count() > 1) {
        /* cpu_call_sync ping: each AP must run smp_ping_fn once. */
        smp_ping_counter = 0;
        for (unsigned i = 1; i < cpu_count(); i++) {
            struct cpu *c = cpu_get(i);
            if (!c || !cpu_online(c))
                continue;
            cpu_call_sync(c, smp_ping_fn, NULL);
        }
        unsigned expect = 0;
        for (unsigned i = 1; i < cpu_count(); i++)
            if (cpu_online(cpu_get(i)))
                expect++;
        SMP_CHECK("cpu_call_sync ping",
                  (unsigned)smp_ping_counter == expect);

        /* Broadcast RESCHEDULE: every AP's ipi_received must advance. */
        unsigned long before[CPU_MAX];
        for (unsigned i = 0; i < cpu_count() && i < CPU_MAX; i++)
            before[i] = cpu_get(i)->ipi_received;
        cpu_broadcast_ipi(IPI_RESCHEDULE);
        uint64_t spins = 0;
        int ok = 0;
        while (spins++ < 50000000ULL) {
            arch_cpu_relax();
            ok = 1;
            for (unsigned i = 1; i < cpu_count(); i++) {
                struct cpu *c = cpu_get(i);
                if (c && cpu_online(c) && c->ipi_received == before[i]) {
                    ok = 0;
                    break;
                }
            }
            if (ok)
                break;
        }
        SMP_CHECK("broadcast IPI received", ok);
    }

    if (fails == 0)
        log_printf(LOG_LEVEL_INFO, "smp: selftest PASS (%u cpu%s)\r\n",
                   cpu_count(), cpu_count() == 1 ? "" : "s");
    else
        log_printf(LOG_LEVEL_ERROR, "smp: selftest FAIL (%d checks)\r\n", fails);
    return fails ? -1 : 0;
#undef SMP_CHECK
}

/* Phase 10: bring up every discovered AP, one at a time. */
int cpu_start_all(void) {
    int online = 0;
    for (unsigned i = 1; i < cpu_nr; i++) {
        if (cpu_start(&cpus[i]) == 0)
            online++;
    }
    return online;
}

/* Phase 6: AP entry after the architecture trampoline dropped us into
 * arch_ap_entry().  Order: per-CPU state -> interrupts -> scheduler ->
 * ONLINE -> idle. */
void cpu_ap_main(struct cpu *cpu) {
    arch_percpu_init(cpu);
    scheduler_init_cpu(cpu);
    cpu_mark_online(cpu);

    log_printf(LOG_LEVEL_INFO, "smp: cpu %u online (hw_id=%llu) cpu_current=%p stack=%p\r\n",
               cpu->id, (unsigned long long)cpu->hw_id,
               (void *)cpu_current(), cpu->kernel_stack);

    for (;;)
        arch_cpu_idle();
}

void cpu_mark_online(struct cpu *cpu) {
    if (!cpu)
        return;
    cpu->state = CPU_ONLINE;
    log_printf(LOG_LEVEL_DEBUG, "smp: cpu %u marked ONLINE\r\n", cpu->id);
}

/* Phase 12: per-CPU scheduler init.  Weak no-op — the scheduler is
 * still global; per-CPU runqueues replace this later. */
__attribute__((weak))
void scheduler_init_cpu(struct cpu *cpu) {
    (void)cpu;
}

/* Phase 9: IPI_STOP target.  During a panic this runs on a CPU that
 * lost the panic race — it must stay silent (the owner has the UART). */
void cpu_stop_self(void) {
    struct cpu *cpu = cpu_current();
    if (cpu) {
        if (!panic_active())
            log_printf(LOG_LEVEL_WARN, "smp: cpu %u stopped\r\n", cpu->id);
        cpu->state = CPU_OFFLINE;
    }
    arch_cpu_stop_self();
}

/* ------------------------------------------------------------------ */
/* IPI                                                                 */
/* ------------------------------------------------------------------ */

void cpu_send_ipi(struct cpu *cpu, enum ipi_type type) {
    if (!cpu || (unsigned)type > (unsigned)IPI_STOP)
        return;
    if (cpu->state != CPU_ONLINE && cpu->state != CPU_STOPPING &&
        type != IPI_STOP)
        return;

    arch_cpu_mark_pending(cpu, (unsigned)type);
    arch_cpu_send_ipi(cpu, (unsigned)type);
}

/* Wake a CPU: if it is online and not us, nudge it to reschedule.
 * Idle CPUs sit in hlt/wfi and only re-evaluate on tick or IPI. */
void cpu_wakeup(struct cpu *cpu) {
    struct cpu *me = cpu_current();
    if (!cpu || cpu == me || !cpu_online(cpu))
        return;
    cpu_send_ipi(cpu, IPI_RESCHEDULE);
}

void cpu_broadcast_ipi(enum ipi_type type) {
    struct cpu *me = cpu_current();
    for (unsigned i = 0; i < cpu_nr; i++) {
        struct cpu *c = &cpus[i];
        if (c == me || !cpu_online(c))
            continue;
        arch_cpu_mark_pending(c, (unsigned)type);
        arch_cpu_send_ipi(c, (unsigned)type);
    }
}

/* Phase 9: single dispatcher.  Arch interrupt code only has to deliver
 * the event here (after collecting the per-CPU pending bits). */
void ipi_handler(enum ipi_type type) {
    switch (type) {
    case IPI_RESCHEDULE:
        scheduler_ipi();
        break;
    case IPI_CALL:
        cpu_call_process();
        break;
    case IPI_TLB:
        tlb_ipi();
        break;
    case IPI_STOP:
        cpu_stop_self();
        break;
    }
}

/* Phase 11: minimal target — flag the current CPU for rescheduling.
 * Real preemption (per-CPU runqueues) lands in Phase 12. */
__attribute__((weak))
void scheduler_ipi(void) {
    struct cpu *cpu = cpu_current();
    if (cpu) {
        cpu->ipi_received++;
        cpu->arch.need_resched = 1;
    }
}

/* Phase 14: TLB shootdown target — arch-specific flush; no-op until
 * the VM layer is wired to it. */
__attribute__((weak))
void tlb_ipi(void) {
    /* Full flush: reload CR3.  No kernel mappings use the GLOBAL bit,
     * so this drops every non-global TLB entry, including stale
     * writable aliases of pages another CPU just made read-only for
     * COW.  Reload via the current directory pointer to stay correct
     * whichever address space is active here. */
    extern void vmm_tlb_reload_current(void);
    vmm_tlb_reload_current();
}

void tlb_flush_others(void) {
    struct cpu *me = cpu_current();
    unsigned n = cpu_count();
    for (unsigned i = 0; i < n; i++) {
        struct cpu *c = cpu_get(i);
        if (!c || c == me || !cpu_online(c))
            continue;
        /* cpu_send_ipi() marks pending itself; no double-mark. */
        cpu_send_ipi(c, IPI_TLB);
    }
}

/* Phase 16: topology decoders.  x86 APIC ID layout (package/core/smt)
 * follows the Intel leaf-0xB decomposition with 4-bit fields as a
 * portable default; ARM MPIDR uses Aff2/Aff1/Aff0.  Good enough until
 * CPUID topology leaves / DT topology bindings are parsed. */
void cpu_topo_decode_apic(struct cpu *cpu, uint32_t apic_id) {
    if (!cpu)
        return;
    cpu->hw_id = apic_id;
    cpu->topo.thread  = apic_id & 0xF;
    cpu->topo.core    = (apic_id >> 4) & 0xF;
    cpu->topo.package = (apic_id >> 8) & 0xFFFFFF;
    cpu->topo.smt = 0;
    /* SMT detection: another CPU with same package+core marks both. */
    for (unsigned i = 0; i < CPU_MAX; i++) {
        struct cpu *o = &cpus[i];
        if (o == cpu || o->state == CPU_OFFLINE)
            continue;
        if (o->topo.package == cpu->topo.package &&
            o->topo.core == cpu->topo.core) {
            o->topo.smt = 1;
            cpu->topo.smt = 1;
        }
    }
}

void cpu_topo_decode_mpidr(struct cpu *cpu, uint64_t mpidr) {
    if (!cpu)
        return;
    cpu->hw_id = mpidr;
    unsigned aff0 = (mpidr >> 0) & 0xFF;
    unsigned aff1 = (mpidr >> 8) & 0xFF;
    unsigned aff2 = (mpidr >> 16) & 0xFF;
    cpu->topo.thread  = aff0;
    cpu->topo.core    = aff1;
    cpu->topo.package = aff2;
    cpu->topo.smt = 0;
    for (unsigned i = 0; i < CPU_MAX; i++) {
        struct cpu *o = &cpus[i];
        if (o == cpu || o->state == CPU_OFFLINE)
            continue;
        if (o->topo.package == cpu->topo.package &&
            o->topo.core == cpu->topo.core) {
            o->topo.smt = 1;
            cpu->topo.smt = 1;
        }
    }
}

int cpu_share_core(const struct cpu *a, const struct cpu *b) {
    if (!a || !b)
        return 0;
    return a->topo.package == b->topo.package &&
           a->topo.core == b->topo.core;
}

int cpu_share_package(const struct cpu *a, const struct cpu *b) {
    if (!a || !b)
        return 0;
    return a->topo.package == b->topo.package;
}

/* ------------------------------------------------------------------ */
/* Cross-CPU calls (Phase 13)                                          */
/* ------------------------------------------------------------------ */

#define CALL_QUEUE_DEPTH 16

struct cpu_call {
    void (*fn)(void *);
    void *arg;
    volatile int *done;   /* for cpu_call_sync, NULL for async */
};

static struct cpu_call call_queue[CPU_MAX][CALL_QUEUE_DEPTH];
static uint32_t call_head[CPU_MAX];   /* next free slot */
static uint32_t call_tail[CPU_MAX];   /* next pending job */
static spinlock_t call_lock[CPU_MAX];

static void cpu_call_enqueue(struct cpu *cpu, void (*fn)(void *),
                               void *arg, volatile int *done) {
    unsigned id = cpu->id;
    uint32_t flags;
    spin_lock_irqsave(&call_lock[id], &flags);

    uint32_t next = (call_head[id] + 1) % CALL_QUEUE_DEPTH;
    if (next == call_tail[id]) {
        spin_unlock_irqrestore(&call_lock[id], flags);
        log_printf(LOG_LEVEL_ERROR, "smp: cpu_call queue full (cpu %u)\r\n", id);
        if (done)
            *done = -1;
        return;
    }

    call_queue[id][call_head[id]].fn   = fn;
    call_queue[id][call_head[id]].arg  = arg;
    call_queue[id][call_head[id]].done = done;
    call_head[id] = next;

    spin_unlock_irqrestore(&call_lock[id], flags);

    cpu_send_ipi(cpu, IPI_CALL);
}

void cpu_call(struct cpu *cpu, void (*fn)(void *), void *arg) {
    if (!cpu || !fn)
        return;

    struct cpu *me = cpu_current();
    if (me == cpu) {
        fn(arg);                     /* local fast path */
        return;
    }
    if (!cpu_online(cpu))
        return;

    cpu_call_enqueue(cpu, fn, arg, NULL);
}

void cpu_call_sync(struct cpu *cpu, void (*fn)(void *), void *arg) {
    if (!cpu || !fn)
        return;
    struct cpu *me = cpu_current();
    if (me == cpu) {
        fn(arg);
        return;
    }
    if (!cpu_online(cpu))
        return;
    volatile int done = 0;
    cpu_call_enqueue(cpu, fn, arg, &done);
    if (done == -1)
        return;  /* queue full */
    uint64_t spins = 0;
    while (!done) {
        arch_cpu_relax();
        if (++spins > 1000000000ULL) {
            log_printf(LOG_LEVEL_ERROR, "smp: cpu_call_sync timeout (cpu %u)\r\n",
                       cpu->id);
            return;
        }
    }
}

void cpu_call_process(void) {
    struct cpu *me = cpu_current();
    if (!me)
        return;

    unsigned id = me->id;
    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&call_lock[id], &flags);
        if (call_tail[id] == call_head[id]) {
            spin_unlock_irqrestore(&call_lock[id], flags);
            break;
        }
        struct cpu_call job = call_queue[id][call_tail[id]];
        call_tail[id] = (call_tail[id] + 1) % CALL_QUEUE_DEPTH;
        spin_unlock_irqrestore(&call_lock[id], flags);

        job.fn(job.arg);
        if (job.done)
            *job.done = 1;
    }
}
