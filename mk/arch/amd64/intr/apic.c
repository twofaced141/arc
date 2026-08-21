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


#include "apic.h"
#include "vmm.h"
#include "debug.h"
#include "acpi.h"
#include "isr.h"
#include "idt.h"


static volatile uint32_t *lapic_base;
static volatile uint32_t *ioapic_base;

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}


int lapic_init(void) {
    uint32_t phys = acpi_info.lapic_addr;
    if (!phys) {
        log_print(LOG_LEVEL_ERROR, "apic: no LAPIC address from ACPI\r\n");
        return -1;
    }

    /* Map LAPIC MMIO page */
    if (vmm_map_page(vmm_get_kernel_directory(),
                     phys, LAPIC_VADDR,
                     VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE) < 0) {
        log_print(LOG_LEVEL_ERROR, "apic: failed to map LAPIC\r\n");
        return -1;
    }
    lapic_base = (volatile uint32_t *)LAPIC_VADDR;

    /* Read version */
    uint32_t version = lapic_read(LAPIC_REG_VERSION);
    uint8_t  max_lvt = (version >> 16) & 0xFF;
    log_printf(LOG_LEVEL_DEBUG, "apic: LAPIC version=0x%x max_lvt=%u\r\n",
                 version & 0xFF, max_lvt);

    /* Mask all LVT entries BEFORE enabling the LAPIC,
       so no stray interrupt can arrive between SVR write
       and LVT configuration. */
    lapic_write(LAPIC_REG_LVT_LINT0,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR,   LAPIC_LVT_MASKED);
    if (max_lvt >= 4)
        lapic_write(LAPIC_REG_LVT_PERF,    LAPIC_LVT_MASKED);
    if (max_lvt >= 5)
        lapic_write(LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);

    /* Set task priority to 0 (accept all) */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Enable LAPIC via Spurious Vector Register.
       Spurious vector 0xFF (unused in IDT).
       Bit 8 = enable. */
    lapic_write(LAPIC_REG_SVR, 0xFF | LAPIC_SVR_ENABLE);

    log_print(LOG_LEVEL_INFO, "apic: LAPIC enabled\r\n");
    return 0;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

/* Busy-wait on the ACPI PM timer (3.579545 MHz); falls back to a
 * countdown loop when no PM timer is present. */
static void apic_delay_us(unsigned us) {
    if (acpi_info.pm_tmr_blk) {
        uint32_t start = inl(acpi_info.pm_tmr_blk);
        uint32_t ticks = (uint32_t)((us * 3579545ULL) / 1000000ULL);
        while ((uint32_t)(inl(acpi_info.pm_tmr_blk) - start) < ticks)
            __asm__ __volatile__("pause");
    } else {
        for (volatile uint64_t i = 0; i < (uint64_t)us * 2000ULL; i++)
            __asm__ __volatile__("pause");
    }
}

/* Calibrate the LAPIC timer bus frequency with a 10 ms one-shot and
 * program a periodic 100 Hz tick on vector 32 (same vector as the
 * BSP's PIT, so the shared IRQ0 handler + scheduler tick logic just
 * work on every CPU).  Runs with IF=0 and the timer LVT masked, so
 * the calibration one-shot can never fire an interrupt. */
void lapic_timer_percpu_init(void) {
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_DIV1);
    lapic_write(LAPIC_REG_TIMER_ICOUNT, 0xFFFFFFFFu);
    apic_delay_us(10000);
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CCOUNT);
    uint32_t per_sec = (0xFFFFFFFFu - remaining) * 100u;
    uint32_t count = per_sec / 100u;
    if (count == 0)
        count = 1;

    lapic_write(LAPIC_REG_TIMER_ICOUNT, count);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | 32);
}

/* ------------------------------------------------------------------ */
/* IPI (Phase 8): physical-mode delivery to one APIC ID               */
/* ------------------------------------------------------------------ */

/* ICR0 delivery modes + flags */
#define ICR_DELIVERY_FIXED  0x000
#define ICR_DELIVERY_INIT   0x500
#define ICR_DELIVERY_SIPI   0x600
#define ICR_PENDING         (1 << 12)
#define ICR_LEVEL_ASSERT    (1 << 14)

static void lapic_send_icr(uint32_t apic_id, uint32_t icr) {
    lapic_write(LAPIC_REG_ICR1, apic_id << 24);        /* physical dest */
    lapic_write(LAPIC_REG_ICR0, icr);
    while (lapic_read(LAPIC_REG_ICR0) & ICR_PENDING)
        ;
}

/* Fixed delivery: used for the generic IPI vector. */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    lapic_send_icr(apic_id, ICR_DELIVERY_FIXED | vector);
}

/* INIT: assert (~10 ms) then deassert. */
void lapic_send_init(uint32_t apic_id) {
    lapic_send_icr(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT);
    apic_delay_us(10000);
    lapic_send_icr(apic_id, ICR_DELIVERY_INIT);        /* deassert */
}

/* SIPI: AP starts executing at vector*0x1000 in real mode. */
void lapic_send_sipi(uint32_t apic_id, uint8_t vector) {
    lapic_send_icr(apic_id, ICR_DELIVERY_SIPI | vector);
    apic_delay_us(200);
}

void lapic_write(int reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

uint32_t lapic_read(int reg) {
    return lapic_base[reg / 4];
}


int ioapic_init(void) {
    uint32_t phys = acpi_info.ioapic_addr;
    if (!phys) {
        log_print(LOG_LEVEL_ERROR, "apic: no I/O APIC address from ACPI\r\n");
        return -1;
    }

    /* Map I/O APIC MMIO page */
    if (vmm_map_page(vmm_get_kernel_directory(),
                     phys, IOAPIC_VADDR,
                     VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE) < 0) {
        log_print(LOG_LEVEL_ERROR, "apic: failed to map I/O APIC\r\n");
        return -1;
    }
    ioapic_base = (volatile uint32_t *)IOAPIC_VADDR;

    /* Read version to get max redirection entry */
    uint32_t ver = ioapic_read(IOAPIC_VERSION);
    uint8_t  max_redir = (ver >> 16) & 0xFF;
    log_printf(LOG_LEVEL_DEBUG, "apic: I/O APIC version=0x%x max_redir=%u\r\n",
                 ver & 0xFF, max_redir);

    return 0;
}

void ioapic_write(int reg, uint32_t val) {
    ioapic_base[IOAPIC_REG_INDEX / 4] = reg;
    ioapic_base[IOAPIC_REG_DATA  / 4] = val;
}

uint32_t ioapic_read(int reg) {
    ioapic_base[IOAPIC_REG_INDEX / 4] = reg;
    return ioapic_base[IOAPIC_REG_DATA / 4];
}

void ioapic_redirect_irq(int irq, uint8_t vector, uint32_t flags) {
    int idx = IOAPIC_REDTABLE + irq * 2;

    /* Low DWORD: vector + flags (delivery mode, polarity, trigger, mask) */
    uint32_t low = vector | flags;

    /* High DWORD: destination (physical mode, BSP is LAPIC ID 0) */
    uint32_t high = 0;

    ioapic_write(idx, low);
    ioapic_write(idx + 1, high);
}

void ioapic_mask_irq(int irq) {
    int idx = IOAPIC_REDTABLE + irq * 2;
    uint32_t low = ioapic_read(idx);
    ioapic_write(idx, low | (1 << 16));  /* set mask bit */
}

void ioapic_unmask_irq(int irq) {
    int idx = IOAPIC_REDTABLE + irq * 2;
    uint32_t low = ioapic_read(idx);
    ioapic_write(idx, low & ~(1 << 16)); /* clear mask bit */
}


/* IOAPIC redirection entry flags from MADT ISO flags */
#define IOAPIC_POLARITY_MASK   0x3
#define IOAPIC_POLARITY_SHIFT  13
#define IOAPIC_TRIGGER_MASK    0xC
#define IOAPIC_TRIGGER_SHIFT   13  /* shift after moving to bit 15 */
#define IOAPIC_POLARITY_HIGH   (0 << 13)
#define IOAPIC_POLARITY_LOW    (1 << 13)
#define IOAPIC_TRIGGER_EDGE    (0 << 15)
#define IOAPIC_TRIGGER_LEVEL   (1 << 15)

void ioapic_setup_isa(void) {
    /* First mask all redirection entries */
    for (int irq = 0; irq < 16; irq++)
        ioapic_mask_irq(irq);

    /* Build override map: default ISA IRQ → GSI is identity (irq == gsi) */
    uint8_t override_gsi[16];
    uint16_t override_flags[16];
    uint8_t has_override[16];
    for (int i = 0; i < 16; i++) {
        override_gsi[i] = (uint8_t)i;
        override_flags[i] = 0;
        has_override[i] = 0;
    }

    for (int i = 0; i < acpi_info.iso_count && i < 16; i++) {
        uint8_t src = acpi_info.isos[i].source;
        if (src < 16) {
            override_gsi[src]   = (uint8_t)acpi_info.isos[i].gsi;
            override_flags[src] = acpi_info.isos[i].flags;
            has_override[src]   = 1;
        }
    }

    /* Track which GSIs have been programmed to detect conflicts */
    uint32_t gsi_used = 0;

    /* Program redirection entries.
       IRQ 0-15 → vector 32-47, same as PIC mapping.
       Base vector = 32.
       Skip IRQ2 (PIC cascade) — meaningless in I/O APIC mode. */
    for (int irq = 0; irq < 16; irq++) {
        if (irq == 2) continue;  /* PIC cascade, not a real ISA IRQ */

        uint8_t gsi = override_gsi[irq];
        uint8_t vec = 32 + irq;
        uint32_t flags = 0;

        if (has_override[irq]) {
            uint16_t madt_flags = override_flags[irq];
            /* Polarity */
            if ((madt_flags & 0x3) == 0x1)
                flags |= IOAPIC_POLARITY_HIGH;
            else if ((madt_flags & 0x3) == 0x3)
                flags |= IOAPIC_POLARITY_LOW;
            /* Trigger mode */
            if ((madt_flags & 0xC) == 0x4)
                flags |= IOAPIC_TRIGGER_EDGE;
            else if ((madt_flags & 0xC) == 0xC)
                flags |= IOAPIC_TRIGGER_LEVEL;
        }

        /* If this GSI was already claimed by a MADT override, skip */
        if ((gsi_used >> gsi) & 1) continue;
        gsi_used |= (1u << gsi);

        ioapic_redirect_irq(gsi, vec, flags);
    }
    log_print(LOG_LEVEL_DEBUG, "apic: ISA IRQs 0-15 mapped to GSIs 0-15 via I/O APIC\r\n");
}


void pic_disable(void) {
    /* Mask all IRQs on master and slave PIC */
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
    log_print(LOG_LEVEL_INFO, "apic: PIC disabled\r\n");
}


int apic_init(void) {
    if (lapic_init() < 0)
        return -1;

    if (ioapic_init() < 0)
        return -1;

    ioapic_setup_isa();

    /* Unmask IRQs we care about: timer (IRQ0 → vector 32), keyboard
       (IRQ1) and cascade from slave (IRQ2).  Others remain masked.
       IRQ1 feeds the userspace PS/2 keyboard driver. */
    ioapic_unmask_irq(0);  /* PIT timer */
    ioapic_unmask_irq(1);  /* PS/2 keyboard (userspace driver) */
    /* IRQ2 is the cascade — must be unmasked for slave IRQs to pass through
       in PIC mode, but with I/O APIC each slave IRQ is independent.
       Unmask IRQ12 (mouse) as needed later. */

    pic_disable();

    log_print(LOG_LEVEL_INFO, "apic: subsystem initialized\r\n");
    return 0;
}
