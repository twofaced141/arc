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
