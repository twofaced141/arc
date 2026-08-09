#ifndef CLOCKEVENT_H
#define CLOCKEVENT_H

#include <stdint.h>

/*
 * Clockevent — a programmable timer device that fires an interrupt at
 * a programmed rate or deadline.  The device's IRQ handler performs
 * hardware maintenance and then calls clockevent_tick() to feed the
 * global tick counter used for uptime/sleep deadlines.
 */

struct clockevent {
    const char *name;
    int irq;                          /* -1 for polled devices */
    void (*set_periodic)(uint32_t hz);   /* program periodic mode */
    void (*set_next_event)(uint64_t ticks); /* one-shot deadline */
};

void clockevent_register(struct clockevent *ce);
uint64_t clockevent_get_ticks(void);
void clockevent_tick(void);

#endif
