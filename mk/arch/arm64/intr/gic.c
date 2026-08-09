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

void gic_init(void) {
    uint32_t type = GICD_TYPER;
    (void)type;

    GICD_CTLR = 1;
    GICC_CTLR = 1;
    GICC_PMR = 0xFF;

    uart_print("gic: initialized\n");
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
