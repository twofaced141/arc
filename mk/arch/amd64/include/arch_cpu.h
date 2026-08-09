#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

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
};

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
