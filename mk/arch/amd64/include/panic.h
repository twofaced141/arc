#ifndef PANIC_H
#define PANIC_H

#include "isr.h"

void panic(const char *reason, const registers_t *r);
void panic_simple(const char *reason);
int panic_active(void);

/* Atomic claim of panic ownership: returns 1 if the caller took it.
 * x86 has native cmpxchg, so __atomic_compare_exchange_n stays inline. */
static inline int panic_claim(int me) {
    extern volatile int panic_owner;
    int expected = -1;
    return __atomic_compare_exchange_n(&panic_owner, &expected, me, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

#endif
