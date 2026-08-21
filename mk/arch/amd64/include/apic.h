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

/* Per-CPU LAPIC timer: calibrate against the PM timer and program a
 * periodic 100 Hz interrupt on vector 32 (APs only — the BSP keeps
 * the PIT). */
void lapic_timer_percpu_init(void);

int  ioapic_init(void);
void ioapic_write(int reg, uint32_t val);
uint32_t ioapic_read(int reg);
void ioapic_redirect_irq(int irq, uint8_t vector, uint32_t flags);
void ioapic_mask_irq(int irq);
void ioapic_unmask_irq(int irq);

void pic_disable(void);

#endif
