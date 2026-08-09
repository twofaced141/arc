#ifndef APIC_H
#define APIC_H

#include <stdint.h>


#define LAPIC_VADDR   0xFFFFFFFFE0000000ULL
#define IOAPIC_VADDR  0xFFFFFFFFE0004000ULL


#define LAPIC_REG_ID       0x020
#define LAPIC_REG_VERSION  0x030
#define LAPIC_REG_TPR      0x080
#define LAPIC_REG_EOI      0x0B0
#define LAPIC_REG_SVR      0x0F0
#define LAPIC_REG_ESR      0x280
#define LAPIC_REG_ICR0     0x300
#define LAPIC_REG_ICR1     0x310
#define LAPIC_REG_LVT_TIMER    0x320
#define LAPIC_REG_LVT_THERMAL  0x330
#define LAPIC_REG_LVT_PERF     0x340
#define LAPIC_REG_LVT_LINT0    0x350
#define LAPIC_REG_LVT_LINT1    0x360
#define LAPIC_REG_LVT_ERROR    0x370
#define LAPIC_REG_TIMER_ICOUNT 0x380
#define LAPIC_REG_TIMER_CCOUNT 0x390
#define LAPIC_REG_TIMER_DIV    0x3E0

/* IPI delivery (Phase 8) */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_send_init(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint8_t vector);

/* LAPIC SVR bits */
#define LAPIC_SVR_ENABLE   (1 << 8)

/* LAPIC LVT delivery mode */
#define LAPIC_LVT_FIXED     0x000
#define LAPIC_LVT_NMI       0x400
#define LAPIC_LVT_EXTINT    0x700

/* LAPIC LVT mask bit */
#define LAPIC_LVT_MASKED    (1 << 16)

/* LAPIC LVT timer mode */
#define LAPIC_TIMER_ONESHOT  0x0000
#define LAPIC_TIMER_PERIODIC 0x20000
#define LAPIC_TIMER_TSCDEADLINE 0x40000

/* LAPIC timer divider */
#define LAPIC_DIV1    0x0B   /* divide by 1 */
#define LAPIC_DIV2    0x00
#define LAPIC_DIV4    0x01
#define LAPIC_DIV8    0x02
#define LAPIC_DIV16   0x03
#define LAPIC_DIV32   0x08
#define LAPIC_DIV64   0x09
#define LAPIC_DIV128  0x0A

/* LAPIC delivery status */
#define LAPIC_ICR_PENDING  (1 << 12)

/* ICR shortcuts */
#define LAPIC_ICR_INIT        0x00000500
#define LAPIC_ICR_STARTUP     0x00000600
#define LAPIC_ICR_LEVEL_ASSERT  0x00004000
#define LAPIC_ICR_LEVEL_DEASSERT 0x00008000
#define LAPIC_ICR_DEST_SELF   0x00040000
#define LAPIC_ICR_DEST_ALL    0x00080000
#define LAPIC_ICR_DEST_OTHERS 0x000C0000

/* I/O APIC register access: index register at offset 0, data at 0x10.
 * IOAPIC base address (physical: 0xFEC00000, virtual: IOAPIC_VADDR).  */
#define IOAPIC_REG_INDEX  0x00
#define IOAPIC_REG_DATA   0x10

/* I/O APIC indirect register indices */
#define IOAPIC_ID         0x00
#define IOAPIC_VERSION    0x01
#define IOAPIC_ARB        0x02
#define IOAPIC_REDTABLE   0x10   /* first redirection entry (2 regs per entry) */


int  apic_init(void);
int  lapic_init(void);
void lapic_eoi(void);
void lapic_write(int reg, uint32_t val);
uint32_t lapic_read(int reg);

int  ioapic_init(void);
void ioapic_write(int reg, uint32_t val);
uint32_t ioapic_read(int reg);
void ioapic_redirect_irq(int irq, uint8_t vector, uint32_t flags);
void ioapic_mask_irq(int irq);
void ioapic_unmask_irq(int irq);

void pic_disable(void);

#endif
