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
        cpus[i].arch.self = &cpus[i];
    }

    cpu_nr = (unsigned)arch_cpu_discover();
    if (cpu_nr == 0)
        cpu_nr = 1;   /* fallback: always at least the BSP */

    /* BSP is alive by definition. */
    cpus[0].state = CPU_ONLINE;
    arch_percpu_init(&cpus[0]);

    log_printf(LOG_LEVEL_INFO, "smp: %u CPU(s) found, boot cpu %u online (hw_id=%u)\r\n",
               cpu_nr, cpus[0].id, cpus[0].hw_id);
}

/* Phase 4 + 7: start one AP.  CPU_STARTING -> arch bringup -> CPU_ONLINE. */
int cpu_start(struct cpu *cpu) {
    if (!cpu || cpu->state != CPU_OFFLINE)
        return -1;

    log_printf(LOG_LEVEL_DEBUG, "smp: starting cpu %u (hw_id=%u)\r\n",
               cpu->id, cpu->hw_id);
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
        if (++spins > 2000000000ULL) {   /* ~seconds; replace with cond-var later */
            log_printf(LOG_LEVEL_ERROR, "smp: cpu %u did not come online (state=%d)\r\n",
                       cpu->id, (int)cpu->state);
            cpu->state = CPU_FAILED;
            return -1;
        }
    }
    return 0;
}

void cpu_stop(struct cpu *cpu) {
    if (!cpu || cpu->state != CPU_ONLINE)
        return;
    cpu->state = CPU_STARTING;   /* brief "dying" phase before STOP */
    cpu_send_ipi(cpu, IPI_STOP);
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

    log_printf(LOG_LEVEL_INFO, "smp: cpu %u online (hw_id=%u) cpu_current=%p stack=%p\r\n",
               cpu->id, cpu->hw_id, (void *)cpu_current(), cpu->kernel_stack);

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

    arch_cpu_mark_pending(cpu, (unsigned)type);
    arch_cpu_send_ipi(cpu, (unsigned)type);
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
    if (cpu)
        cpu->arch.need_resched = 1;
}

/* Phase 14: TLB shootdown target — arch-specific flush; no-op until
 * the VM layer is wired to it. */
__attribute__((weak))
void tlb_ipi(void) {
    /* arch_tlb_flush_local(); */
}

/* ------------------------------------------------------------------ */
/* Cross-CPU calls (Phase 13)                                          */
/* ------------------------------------------------------------------ */

#define CALL_QUEUE_DEPTH 8

struct cpu_call {
    void (*fn)(void *);
    void *arg;
};

static struct cpu_call call_queue[CPU_MAX][CALL_QUEUE_DEPTH];
static uint32_t call_head[CPU_MAX];   /* next free slot */
static uint32_t call_tail[CPU_MAX];   /* next pending job */
static spinlock_t call_lock = SPINLOCK_INIT;

void cpu_call(struct cpu *cpu, void (*fn)(void *), void *arg) {
    if (!cpu || !fn)
        return;

    struct cpu *me = cpu_current();
    if (me == cpu) {
        fn(arg);                     /* local fast path */
        return;
    }

    uint32_t flags;
    spin_lock_irqsave(&call_lock, &flags);

    unsigned id = cpu->id;
    uint32_t next = (call_head[id] + 1) % CALL_QUEUE_DEPTH;
    if (next == call_tail[id]) {
        spin_unlock_irqrestore(&call_lock, flags);
        log_printf(LOG_LEVEL_ERROR, "smp: cpu_call queue full (cpu %u)\r\n", id);
        return;
    }

    call_queue[id][call_head[id]].fn  = fn;
    call_queue[id][call_head[id]].arg = arg;
    call_head[id] = next;

    spin_unlock_irqrestore(&call_lock, flags);

    cpu_send_ipi(cpu, IPI_CALL);
}

void cpu_call_process(void) {
    struct cpu *me = cpu_current();
    if (!me)
        return;

    unsigned id = me->id;
    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&call_lock, &flags);
        if (call_tail[id] == call_head[id]) {
            spin_unlock_irqrestore(&call_lock, flags);
            break;
        }
        struct cpu_call job = call_queue[id][call_tail[id]];
        call_tail[id] = (call_tail[id] + 1) % CALL_QUEUE_DEPTH;
        spin_unlock_irqrestore(&call_lock, flags);

        job.fn(job.arg);
    }
}
