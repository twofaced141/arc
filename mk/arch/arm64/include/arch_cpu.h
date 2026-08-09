#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

struct cpu;

/* Per-CPU arch state (AArch64).  TPIDR_EL1 points at the start of this
 * structure during kernel execution (EL1 only; user TPIDR is separate),
 * so cpu_current() is a TPIDR_EL1 load + one pointer dereference. */
struct arch_cpu {
    struct cpu *self;           /* offset 0: cpu_current() reads TPIDR_EL1[0] */
    uint64_t mpidr;             /* hw id (MPIDR_EL1 / DT cpu reg) */
    uint32_t ipi_pending;       /* bitmask of pending IPI types */
    int      need_resched;      /* Phase 11: set by scheduler_ipi() */
    uint64_t stack_base;        /* per-CPU kernel stack */
};

static inline struct cpu *arch_cpu_current(void) {
    struct cpu *self;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(self));
    return self;
}

static inline void arch_cpu_relax(void) {
    __asm__ __volatile__("yield");
}

/* Phase 8: mark a pending-IPI bit before sending the IPI. */
void arch_cpu_mark_pending(struct cpu *cpu, unsigned type);

/* Phase 1: fill cpus[] from Device Tree (and later ACPI MADT GICC).
 * Returns count, 0 on failure. */
int arch_cpu_discover(void);

/* Phase 3: point TPIDR_EL1 at cpu->arch and prepare per-CPU state. */
int arch_percpu_init(struct cpu *cpu);

/* Phase 4: PSCI CPU_ON bringup of one AP. */
int arch_cpu_start(struct cpu *cpu);

/* Phase 8: architecture-specific IPI send (GIC SGI). */
void arch_cpu_send_ipi(struct cpu *cpu, unsigned type);

/* Phase 6: idle loop on the AP. */
void arch_cpu_idle(void);

/* Phase 9: stop this CPU (IPI_STOP / cpu_stop). */
void arch_cpu_stop_self(void);

/* AP trampoline entry (arch_cpu_start target). */
void arch_ap_entry(struct cpu *cpu);

/* Install the architecture IPI delivery (IDT vector / GIC SGI). */
void ipi_init(void);

#endif
