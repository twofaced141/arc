#ifndef CLOCKSOURCE_H
#define CLOCKSOURCE_H

#include <stdint.h>

/*
 * Clocksource — a free-running, readable monotonic counter used for
 * timekeeping (Linux-style split: clocksource = "read the clock",
 * clockevent = "programmable interrupt source").
 */

struct clocksource {
    const char *name;
    uint32_t freq;            /* counter frequency in Hz */
    uint64_t mask;            /* valid counter bits, 0 = full 64-bit */
    uint64_t (*read)(void);   /* raw counter read */
};

void clocksource_register(struct clocksource *cs);
struct clocksource *clocksource_current(void);
uint64_t clocksource_read(void);
uint64_t clocksource_read_ns(void);
uint32_t clocksource_freq(void);

#endif
