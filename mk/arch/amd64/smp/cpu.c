/* x86_64 SMP: CPU discovery (MADT), per-CPU GS access, INIT/SIPI
 * bringup, LAPIC IPI backend.  Generic logic lives in mk/smp/cpu.c. */
#include "cpu.h"
#include "acpi.h"
#include "apic.h"
#include "vmm.h"
#include "pmm.h"
#include "idt.h"
#include "isr.h"
#include "gdt.h"
#include "string.h"
#include "debug.h"

/* ------------------------------------------------------------------ */
/* Trampoline (Phase 5)                                                */
/* ------------------------------------------------------------------ */

#define TRAMPOLINE_BASE   0x6000
#define TRAMPOLINE_VECTOR 0x06

extern uint8_t _trampoline_start[];
extern uint8_t _trampoline_end[];

#define TRAMP_OFF_GDT    0x100
#define TRAMP_OFF_IDT    0x10A
#define TRAMP_OFF_CR3    0x118
#define TRAMP_OFF_STACK  0x120
#define TRAMP_OFF_GS     0x128
#define TRAMP_OFF_ENTRY  0x130
#define TRAMP_OFF_CPU    0x138

#define IPI_VECTOR 0x4F

static int trampoline_loaded;

/* Copy the trampoline image to TRAMPOLINE_BASE and fill its data
 * fields.  0x6000 is inside the identity map (0-64MB, writable), so
 * plain stores work from any context. */
static int trampoline_setup(void) {
    if (trampoline_loaded)
        return 0;

    memcpy((void *)(uintptr_t)TRAMPOLINE_BASE, _trampoline_start,
           (size_t)(_trampoline_end - _trampoline_start));

    uint64_t gdt_base, idt_base;
    uint16_t gdt_limit, idt_limit;
    gdt_get_ptr(&gdt_base, &gdt_limit);
    idt_get_ptr(&idt_base, &idt_limit);

    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE_BASE;
    *(uint16_t *)(t + TRAMP_OFF_GDT)     = gdt_limit;
    *(uint64_t *)(t + TRAMP_OFF_GDT + 2) = gdt_base;
    *(uint16_t *)(t + TRAMP_OFF_IDT)     = idt_limit;
    *(uint64_t *)(t + TRAMP_OFF_IDT + 2) = idt_base;
    *(uint64_t *)(t + TRAMP_OFF_CR3)     = (uint64_t)(uintptr_t)vmm_get_kernel_directory();

    trampoline_loaded = 1;
    log_print(LOG_LEVEL_DEBUG, "smp: trampoline loaded at 0x6000\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase 1: discovery from ACPI MADT                                   */
/* ------------------------------------------------------------------ */

int arch_cpu_discover(void) {
    int n = 0;

    /* MADT Processor Local APIC entries (enabled only).  Kernel CPU id
     * is the array index — deliberately NOT the APIC ID. */
    if (acpi_info.valid && acpi_info.lapic_count > 0) {
        for (int i = 0; i < acpi_info.lapic_count && n < CPU_MAX; i++) {
            if (!(acpi_info.lapics[i].flags & 1))
                continue;   /* disabled */
            struct cpu *c = &cpus[n];
            c->id = (unsigned)n;
            c->hw_id = acpi_info.lapics[i].apic_id;
            c->arch.apic_id = acpi_info.lapics[i].apic_id;
            log_printf(LOG_LEVEL_DEBUG, "smp: cpu %d -> apic id %u\r\n",
                       n, acpi_info.lapics[i].apic_id);
            n++;
        }
    }

    if (n == 0) {
        /* No MADT / no entries: single-CPU fallback. */
        cpus[0].id = 0;
        cpus[0].hw_id = 0;
        cpus[0].arch.apic_id = 0;
        n = 1;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Phase 3: per-CPU access via GS.base                                 */
/* ------------------------------------------------------------------ */

int arch_percpu_init(struct cpu *cpu) {
    cpu->arch.self = cpu;
    cpu->arch.ipi_pending = 0;
    cpu->arch.need_resched = 0;

    /* GS.base = &cpu->arch — cpu_current() is %gs:0.  No swapgs in the
     * kernel (user TLS uses FS), so this survives user/kernel crosses. */
    uint32_t lo = (uint32_t)(uintptr_t)&cpu->arch;
    uint32_t hi = (uint32_t)((uintptr_t)&cpu->arch >> 32);
    __asm__ __volatile__("wrmsr" : : "c"((uint32_t)0xC0000101), "a"(lo), "d"(hi));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase 4 + 5: INIT/SIPI bringup                                      */
/* ------------------------------------------------------------------ */

void arch_ap_entry(struct cpu *cpu) {
    /* Long mode, per-CPU stack, GS set, shared GDT/IDT. */
    lapic_init();   /* INIT left the LAPIC disabled — enables SVR, masks LVTs */
    cpu_ap_main(cpu);
    /* never returns */
}

int arch_cpu_start(struct cpu *cpu) {
    if (trampoline_setup() < 0)
        return -1;

    /* Per-CPU kernel stack (identity-mapped). */
    uint8_t *stack = (uint8_t *)pmm_alloc_pages(THREAD_KSTACK_SIZE / PAGE_SIZE);
    if (!stack)
        return -1;
    cpu->kernel_stack = stack;

    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE_BASE;
    *(uint64_t *)(t + TRAMP_OFF_STACK) = (uint64_t)(uintptr_t)(stack + THREAD_KSTACK_SIZE);
    *(uint64_t *)(t + TRAMP_OFF_GS)    = (uint64_t)(uintptr_t)&cpu->arch;
    *(uint64_t *)(t + TRAMP_OFF_ENTRY) = (uint64_t)(uintptr_t)arch_ap_entry;
    *(uint64_t *)(t + TRAMP_OFF_CPU)   = (uint64_t)(uintptr_t)cpu;

    /* INIT -> 10ms -> deassert -> 2x SIPI (Phase 4 handshake). */
    lapic_send_init(cpu->arch.apic_id);
    lapic_send_sipi(cpu->arch.apic_id, TRAMPOLINE_VECTOR);
    lapic_send_sipi(cpu->arch.apic_id, TRAMPOLINE_VECTOR);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase 8 + 9: IPI backend                                            */
/* ------------------------------------------------------------------ */

void arch_cpu_mark_pending(struct cpu *cpu, unsigned type) {
    __atomic_or_fetch(&cpu->arch.ipi_pending, 1u << type, __ATOMIC_RELEASE);
}

void arch_cpu_send_ipi(struct cpu *cpu, unsigned type) {
    (void)type;
    lapic_send_ipi(cpu->arch.apic_id, IPI_VECTOR);
}

/* IRQ handler: collect pending IPI bits and dispatch to the generic
 * ipi_handler(), then EOI. */
static void ipi_irq_handler(registers_t *r) {
    (void)r;
    struct cpu *me = cpu_current();
    if (!me) {
        lapic_eoi();
        return;
    }

    uint32_t pending = __atomic_exchange_n(&me->arch.ipi_pending, 0, __ATOMIC_ACQUIRE);
    unsigned type = 0;
    while (pending) {
        if (pending & 1)
            ipi_handler((enum ipi_type)type);
        pending >>= 1;
        type++;
    }
    lapic_eoi();
}

void ipi_init(void) {
    extern void irq_vector(void);
    extern void irq79(void);
    (void)irq_vector;
    idt_set_gate(IPI_VECTOR, (uint64_t)irq79, 0x08, 0x8E);
    register_interrupt_handler(IPI_VECTOR, ipi_irq_handler);
    log_print(LOG_LEVEL_DEBUG, "smp: IPI vector 0x4F installed\r\n");
}

/* ------------------------------------------------------------------ */
/* Idle / stop                                                         */
/* ------------------------------------------------------------------ */

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
