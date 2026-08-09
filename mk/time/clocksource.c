#include "clocksource.h"

static struct clocksource *current_cs;
static uint64_t cs_mult;
static unsigned cs_shift;

void clocksource_register(struct clocksource *cs) {
    current_cs = cs;
    cs_shift = 10;
    cs_mult = (1000000000ULL << cs_shift) / cs->freq;
    if (cs_mult == 0) {
        cs_shift = 0;
        cs_mult = 1000000000ULL / cs->freq;
    }
}

struct clocksource *clocksource_current(void) {
    return current_cs;
}

uint64_t clocksource_read(void) {
    return current_cs ? current_cs->read() : 0;
}

uint64_t clocksource_read_ns(void) {
    uint64_t counter = clocksource_read();
    if (current_cs && current_cs->mask)
        counter &= current_cs->mask;
    return (counter * cs_mult) >> cs_shift;
}

uint32_t clocksource_freq(void) {
    return current_cs ? current_cs->freq : 0;
}
