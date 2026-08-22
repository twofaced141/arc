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


#include "clkevt_arm.h"
#include "clocksource.h"
#include "clockevent.h"
#include "gic.h"
#include "uart.h"
#include "isr.h"

/* arm64 generic timer — clockevent: CNTV timer, periodic IRQ. */

static uint32_t timer_period;

static void arm_set_periodic(uint32_t hz) {
    uint64_t freq;

    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
    timer_period = (uint32_t)(freq / hz);

    __asm__ __volatile__(
        "msr cntv_tval_el0, %[period]\n"
        "msr cntv_ctl_el0, %[enable]\n"
        :
        : [period] "r"(timer_period),
          [enable] "r"(1UL)
        : "memory");
}

static void arm_set_next_event(uint64_t ticks) {
    (void)ticks;
    /* one-shot mode not used yet */
}

static void timer_handler(registers_t *r) {
    (void)r;

    clockevent_tick();

    __asm__ __volatile__(
        "msr cntv_ctl_el0, %[disable]\n"
        "isb\n"
        "msr cntv_tval_el0, %[period]\n"
        "isb\n"
        "msr cntv_ctl_el0, %[enable]\n"
        "isb\n"
        :
        : [disable] "r"(0UL),
          [period] "r"(timer_period),
          [enable] "r"(1UL)
        : "memory");
}

static struct clockevent arm_clockevent = {
    .name = "arm-generic-timer",
    .irq = TIMER_IRQ,
    .set_periodic = arm_set_periodic,
    .set_next_event = arm_set_next_event,
};

void clkevt_arm_init(void) {
    register_interrupt_handler(TIMER_IRQ, timer_handler);
    gic_enable_irq(TIMER_IRQ);

    clockevent_register(&arm_clockevent);
    arm_set_periodic(100);

    uart_print("timer: init done, ~100Hz\n");
}

void clkevt_arm_cpu_init(void) {
    /* PPI IRQs are per-CPU banked in GICv2; enable on this CPU and
     * reprogram the generic timer. timer_period already set by BSP. */
    gic_enable_irq(TIMER_IRQ);
    if (timer_period == 0) {
        uint64_t freq;
        __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
        timer_period = (uint32_t)(freq / 100);
    }
    __asm__ __volatile__(
        "msr cntv_tval_el0, %0\n"
        "msr cntv_ctl_el0, %1\n"
        :
        : "r"(timer_period), "r"(1UL)
        : "memory");
}
