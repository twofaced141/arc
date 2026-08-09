#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

struct cpu;

/* Per-CPU arch state (i386).  The kernel is not (yet) per-CPU aware on
 * i386 — the fields exist so generic code compiles; cpu_current()
 * always returns CPU 0. */
struct arch_cpu {
    struct cpu *self;
    uint32_t apic_id;
    uint32_t ipi_pending;
    int      need_resched;
    uint64_t stack_base;
};

static inline struct cpu *arch_cpu_current(void) {
    struct cpu *self;
    __asm__ __volatile__("movl %%fs:0, %0" : "=r"(self));
    return self;
}

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
