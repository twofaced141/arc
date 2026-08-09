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


#include "clksrc_arm.h"
#include "clocksource.h"

/* arm64 generic timer — clocksource: free-running CNTVCT counter. */

static uint64_t arm_read_virt_count(void) {
    uint64_t val;
    __asm__ __volatile__(
        "isb\n"
        "mrs %0, cntvct_el0\n"
        : "=r"(val));
    return val;
}

uint64_t timer_read_phys_count(void) {
    uint64_t val;
    __asm__ __volatile__(
        "isb\n"
        "mrs %0, cntpct_el0\n"
        : "=r"(val));
    return val;
}

uint64_t timer_read_virt_count(void) {
    return arm_read_virt_count();
}

static struct clocksource arm_clocksource = {
    .name = "arm-generic-timer",
    .freq = 0,   /* filled in clksrc_arm_init from CNTFRQ */
    .mask = 0,   /* full 64-bit counter */
    .read = arm_read_virt_count,
};

void clksrc_arm_init(void) {
    uint64_t freq;

    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
    arm_clocksource.freq = (uint32_t)freq;

    clocksource_register(&arm_clocksource);
}
