#ifndef APIC_H
#define APIC_H

#include <stdint.h>

#define LAPIC_PHYS_BASE  0xFEE00000
#define IOAPIC_PHYS_BASE 0xFEC00000
#define LAPIC_VADDR      0xFFE00000
#define IOAPIC_VADDR     0xFFE01000

#define IA32_APIC_BASE   0x1B

#define LAPIC_ID         0x020
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0
#define LAPIC_TPR        0x080
#define LAPIC_DFR        0x0E0
#define LAPIC_LDR        0x0D0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PM     0x340
#define LAPIC_LVT_LINT0  0x350
#define LAPIC_LVT_LINT1  0x360
#define LAPIC_LVT_ERROR  0x370
#define LAPIC_TIMER_ICR  0x380
#define LAPIC_TIMER_CCR  0x390
#define LAPIC_TIMER_DCR  0x3E0
#define LAPIC_ICR_LOW    0x300
#define LAPIC_ICR_HIGH   0x310

#define LAPIC_SVR_ENABLE (1 << 8)
#define LAPIC_LVT_MASKED (1 << 16)
#define LAPIC_LVT_PERIODIC (1 << 17)
#define LAPIC_LVT_TIMER_MODE_ONESHOT (0 << 17)
#define LAPIC_LVT_TIMER_MODE_PERIODIC (1 << 17)

#define LAPIC_TIMER_DIV1    0x0B
#define LAPIC_TIMER_DIV2    0x00
#define LAPIC_TIMER_DIV4    0x01
#define LAPIC_TIMER_DIV8    0x02
#define LAPIC_TIMER_DIV16   0x03
#define LAPIC_TIMER_DIV32   0x08
#define LAPIC_TIMER_DIV64   0x09
#define LAPIC_TIMER_DIV128  0x0A

#define IOAPIC_IOREGSEL 0x00
#define IOAPIC_IOWIN    0x10
#define IOAPIC_ID       0x00
#define IOAPIC_VER      0x01
#define IOAPIC_ARB      0x02
#define IOAPIC_REDTBL   0x10

#define IOAPIC_RED_MASK      (1 << 16)
#define IOAPIC_RED_FIXED     (0 << 8)
#define IOAPIC_RED_LOWPRI    (1 << 8)
#define IOAPIC_RED_PHYSICAL  (0 << 11)
#define IOAPIC_RED_LOGICAL   (1 << 11)
#define IOAPIC_RED_EDGE      (0 << 15)
#define IOAPIC_RED_LEVEL     (1 << 15)
#define IOAPIC_RED_ACTIVEHIGH (0 << 13)
#define IOAPIC_RED_ACTIVELOW  (1 << 13)

extern int apic_enabled;

uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t val);
void     lapic_init(void);
void     lapic_eoi(void);
void     lapic_timer_init(void);

void     ioapic_init(void);
void     ioapic_mask_pic(void);

#endif
