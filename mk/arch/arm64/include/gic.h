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


#ifndef GIC_H
#define GIC_H

#include <stdint.h>

#define GICD_BASE   0x08000000
#define GICC_BASE   0x08010000

#define GICD_CTLR          (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_TYPER         (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_IIDR          (*(volatile uint32_t *)(GICD_BASE + 0x008))
#define GICD_ISENABLER(n)  (*(volatile uint32_t *)(GICD_BASE + 0x100 + (n) * 4))
#define GICD_ICENABLER(n)  (*(volatile uint32_t *)(GICD_BASE + 0x180 + (n) * 4))
#define GICD_IPRIORITYR(n) (*(volatile uint32_t *)(GICD_BASE + 0x400 + (n) * 4))
#define GICD_ITARGETSR(n)  (*(volatile uint32_t *)(GICD_BASE + 0x800 + (n) * 4))
#define GICD_ICPENDR(n)    (*(volatile uint32_t *)(GICD_BASE + 0x280 + (n) * 4))

#define GICC_CTLR   (*(volatile uint32_t *)(GICC_BASE + 0x0000))
#define GICC_PMR    (*(volatile uint32_t *)(GICC_BASE + 0x0004))
#define GICC_IAR    (*(volatile uint32_t *)(GICC_BASE + 0x000C))
#define GICC_EOIR   (*(volatile uint32_t *)(GICC_BASE + 0x0010))

#define GICC_PMR_PRIO 0xFF

void gic_init(void);
void gic_enable_irq(uint32_t irq);
void gic_disable_irq(uint32_t irq);
void gic_eoi(uint32_t irq);

#endif
