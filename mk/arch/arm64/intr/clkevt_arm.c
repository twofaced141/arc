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
