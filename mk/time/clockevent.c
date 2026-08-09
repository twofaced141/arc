#include "clockevent.h"

static struct clockevent *current_ce;
static volatile uint64_t ticks;

void clockevent_register(struct clockevent *ce) {
    current_ce = ce;
}

uint64_t clockevent_get_ticks(void) {
    return ticks;
}

void clockevent_tick(void) {
    ticks++;
}
