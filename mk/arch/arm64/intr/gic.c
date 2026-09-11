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


#include "gic.h"
#include "uart.h"

static int gic_v3 = -1;

extern uint64_t boot_dtb_ptr;

static int dtb_has_gicv3(void) {
    /* Search the DTB blob for a "arm,gic-v3" compatible string.  The
     * distributor version is a DT property — PFR0 only tells whether
     * the *CPU* supports v3 sysregs (Cortex-A57 does even when the
     * virt machine provides a v2 distributor, where ICC_SGI1R goes
     * nowhere).  A raw memmem over the blob is crude but robust. */
    const uint8_t *dtb = (const uint8_t *)(uintptr_t)boot_dtb_ptr;
    if (!dtb)
        return 0;
    /* totalsize at offset 4 (big-endian); clamp the scan. */
    uint32_t total = ((uint32_t)dtb[4] << 24) | ((uint32_t)dtb[5] << 16) |
                     ((uint32_t)dtb[6] << 8) | (uint32_t)dtb[7];
    if (total < 64 || total > 0x200000)
        return 0;
    static const char needle[] = "arm,gic-v3";
    for (uint32_t i = 0; i + sizeof(needle) <= total; i++) {
        uint32_t j = 0;
        while (j < sizeof(needle) - 1 && dtb[i + j] == (uint8_t)needle[j])
            j++;
        if (j == sizeof(needle) - 1)
            return 1;
    }
    return 0;
}

int gic_is_v3(void) {
    if (gic_v3 >= 0)
        return gic_v3;
    /* Distributor version comes from DT; PFR0 only gates whether the
     * CPU *can* drive v3 sysregs.  Default to v2 (QEMU virt default). */
    if (!dtb_has_gicv3()) {
        gic_v3 = 0;
        return 0;
    }
    uint64_t pfr0;
    __asm__ __volatile__("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
    if (((pfr0 >> 24) & 0xF) == 0) {
        gic_v3 = 0;
        return 0;
    }
    /* Enable SRE so ICC_SGI1R works. */
    uint64_t sre;
    __asm__ __volatile__("mrs %0, icc_sre_el1" : "=r"(sre));
    __asm__ __volatile__("msr icc_sre_el1, %0" :: "r"(sre | 1ULL) : "memory");
    __asm__ __volatile__("isb" ::: "memory");
    gic_v3 = 1;
    return 1;
}

void gic_send_sgi(uint64_t mpidr, unsigned sgi) {
    sgi &= 0xF;
    if (gic_is_v3()) {
        /* ICC_SGI1R_EL1: Aff3[55:48] Aff2[39:32] Aff1[23:16] RS[43:40]
         * target-list[15:0] SGI[3:0].  Target this exact affinity. */
        unsigned aff0 = (mpidr >> 0) & 0xFF;
        unsigned aff1 = (mpidr >> 8) & 0xFF;
        unsigned aff2 = (mpidr >> 16) & 0xFF;
        unsigned aff3 = (mpidr >> 32) & 0xFF;
        uint64_t v = ((uint64_t)aff3 << 48) |
                     ((uint64_t)aff2 << 32) |
                     ((uint64_t)aff1 << 16) |
                     ((uint64_t)1 << aff0) |
                     ((uint64_t)sgi);
        __asm__ __volatile__("msr icc_sgi1r_el1, %0" :: "r"(v) : "memory");
        __asm__ __volatile__("isb" ::: "memory");
        return;
    }
    /* GICv2: SGIR CPUTargetList is a bitmask of Aff0 CPUs (8 max);
     * clusters / >8 CPUs unreachable.  NOTE: the old code wrote the
     * Aff0 *value* instead of a bitmask, so every IPI went to CPU0. */
    if (((mpidr >> 8) & 0xFFFFFF) != 0) {
        uart_print("gic: WARN SGI to non-Aff0 mpidr unreachable on GICv2\n");
        return;
    }
    unsigned aff0 = mpidr & 0x7;
    volatile uint32_t *sgir =
        (volatile uint32_t *)(uintptr_t)(GICD_BASE + 0xF00);
    *sgir = (1 << 15) | ((1u << aff0) << 16) | sgi;
}

void gic_init(void) {
    uint32_t type = GICD_TYPER;
    (void)type;

    GICD_CTLR = 1;
    GICC_CTLR = 1;
    GICC_PMR = 0xFF;

    (void)gic_is_v3();
    uart_print(gic_v3 ? "gic: initialized (v3)\n" : "gic: initialized (v2)\n");
}

void gic_cpu_init(void) {
    GICC_CTLR = 1;
    GICC_PMR = 0xFF;
}

void gic_enable_irq(uint32_t irq) {
    if (irq > 1019) return;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    uint32_t prio_reg = irq / 4;
    uint32_t prio_shift = (irq % 4) * 8;

    GICD_IPRIORITYR(prio_reg) &= ~(0xFF << prio_shift);
    GICD_IPRIORITYR(prio_reg) |= (0x80 << prio_shift);
    GICD_ISENABLER(reg) = (1u << bit);
}

void gic_disable_irq(uint32_t irq) {
    if (irq > 1019) return;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    GICD_ICENABLER(reg) = (1u << bit);
}

void gic_eoi(uint32_t irq) {
    GICC_EOIR = irq;
}
