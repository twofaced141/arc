#ifndef __aarch64__
/* pit.c — x86 (amd64/i386) only; arm64 uses intr/clksrc_arm.c +
 * intr/clkevt_arm.c.  Registers the PIT as both a clockevent (periodic
 * IRQ feeding the tick counter) and a clocksource (latch read of the
 * free-running channel-0 down counter). */

#include "pit.h"
#include "idt.h"
#include "isr.h"
#include "clocksource.h"
#include "clockevent.h"
#include "debug.h"

#define PIT_BASE_FREQ 1193180u
#define PIT_IRQ       32

static uint32_t pit_divisor = 11931; /* 100 Hz default */

static void pit_set_periodic(uint32_t hz) {
    pit_divisor = PIT_BASE_FREQ / hz;
    outb(0x43, 0x36);  /* Command: channel 0, lobyte/hibyte, mode 3, binary */
    outb(0x40, pit_divisor & 0xFF);
    outb(0x40, (pit_divisor >> 8) & 0xFF);
}

static void pit_set_next_event(uint64_t ticks) {
    (void)ticks;
    /* one-shot mode not used yet */
}

static void timer_handler(registers_t *r) {
    (void)r;
    clockevent_tick();
}

static struct clockevent pit_clockevent = {
    .name = "pit",
    .irq = PIT_IRQ,
    .set_periodic = pit_set_periodic,
    .set_next_event = pit_set_next_event,
};

static uint64_t pit_read_counter(void) {
    uint32_t lo, hi;

    outb(0x43, 0x00);  /* Latch channel 0 */
    lo = inb(0x40);
    hi = inb(0x40);

    /* The PIT counts down from the reload value and wraps; convert to
     * an incrementing counter of the same period. */
    return (uint64_t)(pit_divisor - (uint32_t)((hi << 8) | lo));
}

static struct clocksource pit_clocksource = {
    .name = "pit",
    .freq = PIT_BASE_FREQ,
    .mask = 0xFFFF,
    .read = pit_read_counter,
};

void pit_init(void) {
    pit_set_periodic(100);
    register_interrupt_handler(PIT_IRQ, timer_handler);

    clockevent_register(&pit_clockevent);
    clocksource_register(&pit_clocksource);

    log_print(LOG_LEVEL_DEBUG, "pit: initialized at ~100Hz\r\n");
}

#endif /* !__aarch64__ */
