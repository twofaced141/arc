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
