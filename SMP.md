# SMP — план реализации (x86_64 / arm64)

Цель: generic kernel (scheduler/MM/code) не содержит x86/ARM-specific SMP логики.
Архитектура реализует только: discovery, per-CPU access, startup, IPI backend.

## Phase 0 — Архитектура (API)

```c
struct cpu;

struct cpu *cpu_current(void);
struct cpu *cpu_get(unsigned id);

unsigned cpu_count(void);
bool cpu_online(struct cpu *);

int cpu_start(struct cpu *);
void cpu_stop(struct cpu *);
```

IPI:
```c
enum ipi_type {
    IPI_RESCHEDULE,
    IPI_CALL,
    IPI_TLB,
    IPI_STOP,
};

void cpu_send_ipi(struct cpu *, enum ipi_type);
```

Cross-CPU call:
```c
void cpu_call(struct cpu *,
              void (*fn)(void *),
              void *arg);
```

## Phase 1 — CPU discovery

- x86_64: ACPI MADT (Processor Local APIC entries). Kernel CPU ID != APIC ID.
- ARM64: ACPI MADT (GICC) и/или Device Tree.
- Результат: `cpu_count() == N`, массив `struct cpu cpus[N]`.

## Phase 2 — struct cpu (per-CPU state)

Generic: id, hw_id, online, current, idle, runqueue, kernel_stack.
Arch: APIC ID / MPIDR, GDT, TSS, exception state, registers, ...

## Phase 3 — Per-CPU access

- x86_64: GS.base → struct cpu
- ARM64: TPIDR_EL1 → struct cpu
- Per-CPU data работает ещё до запуска scheduler на AP.

## Phase 4 — CPU startup abstraction

`int arch_cpu_start(struct cpu *);`
- x86: BSP → stack/per-CPU/trampoline → INIT IPI → SIPI → SIPI → AP
- ARM64: BSP → PSCI CPU_ON / platform mechanism → AP

## Phase 5 — x86 AP trampoline

`arch/x86_64/smp/ap_trampoline.S`, максимально маленький:
AP starts → minimal CPU setup → stack → GDT → long mode → GS/per-CPU →
`void arch_ap_entry(struct cpu *cpu);`

## Phase 6 — AP initialization

```c
void cpu_ap_main(struct cpu *cpu)
{
    arch_cpu_init();
    percpu_init(cpu);
    interrupt_init_cpu();
    scheduler_init_cpu(cpu);
    cpu_mark_online(cpu);
    cpu_idle();
}
```

## Phase 7 — CPU startup handshake

```c
enum cpu_state { CPU_OFFLINE, CPU_STARTING, CPU_ONLINE, CPU_FAILED };
```

BSP ждёт `while (!cpu_online(cpu)) cpu_relax();` (позже — нормальный примитив).

## Phase 8 — IPI subsystem

Generic enum; архитектура реализует отправку:
- x86: generic IPI → LAPIC → interrupt vector
- ARM64: generic IPI → GIC → SGI

## Phase 9 — IPI handler

```c
void ipi_handler(enum ipi_type type)
{
    switch (type) {
    case IPI_RESCHEDULE: scheduler_ipi(); break;
    case IPI_CALL:       cpu_call_process(); break;
    case IPI_TLB:        tlb_ipi(); break;
    case IPI_STOP:       cpu_stop_self(); break;
    }
}
```

Arch interrupt code только доставляет событие до этого уровня.

## Phase 10 — Первый SMP тест

CPU 0 стартует CPU 1..N; каждый CPU пишет cpu_current(), hw_id, stack,
per-CPU address. Проверить: два CPU не используют один struct cpu / stack.

## Phase 11 — IPI_RESCHEDULE

CPU 0 → cpu_send_ipi(CPU1, IPI_RESCHEDULE) → LAPIC/GIC → handler → scheduler_ipi().

## Phase 12 — Scheduler per CPU

CPU i → runqueue i; каждый CPU исполняет свои idle/threads независимо.
Load balancing — НЕ сразу.

## Phase 13 — Cross-CPU calls

cpu_call(cpu, fn, arg): enqueue fn + IPI_CALL → dequeue → execute.
Фундамент для следующих подсистем.

## Phase 14 — TLB shootdown

Изменение page tables → local invalidation → IPI_TLB всем. Generic VM не знает
про INVLPG / TLBI / CR3 / TLBI ASIDE1.

## Phase 15 — Scheduler load balancing

thread_migrate(thread, cpu); cpu_wakeup(cpu); IPI_RESCHEDULE как уведомление.

## Phase 16 — Topology

package/core/SMT (x86: APIC ID), cluster/core/thread (ARM64: MPIDR).
Scheduler получает абстракцию topology.

## Phase 17 — Power / CPU hotplug (НЕ часть первоначального SMP)

cpu_online()/cpu_offline()/cpu_suspend()/cpu_resume().
x86: APIC/INIT/platform; ARM64: PSCI/platform.

## Приоритет реализации (backlog)

- [x] ACPI/DT CPU discovery
- [x] struct cpu
- [x] cpu_current()
- [x] x86 GS per-CPU
- [x] ARM64 TPIDR_EL1 per-CPU
- [x] x86 AP trampoline
- [x] ARM64 secondary CPU startup
- [x] INIT/SIPI
- [x] CPU startup handshake
- [x] AP initialization
- [x] CPU_ONLINE
- [x] generic IPI API
- [x] x86 LAPIC IPI backend
- [x] ARM64 GIC IPI backend
- [x] IPI_RESCHEDULE
- [x] per-CPU runqueues
- [x] scheduler on all CPUs
- [x] cpu_call()
- [x] IPI_CALL
- [x] TLB shootdown
- [x] IPI_TLB
- [x] thread migration
- [x] load balancing
- [x] CPU topology
- [ ] CPU hotplug
