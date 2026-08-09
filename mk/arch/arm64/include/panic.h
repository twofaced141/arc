#ifndef PANIC_H
#define PANIC_H

#include "isr.h"

void panic(const char *reason, const registers_t *r);
void panic_simple(const char *reason);
int panic_active(void);

/* Atomic claim of panic ownership: returns 1 if the caller took it.
 * At -O0 GCC routes __atomic_compare_exchange_n through libatomic on
 * aarch64 without LSE (__aarch64_cas4_acq_rel) — inline ldxr/stlxr. */
static inline int panic_claim(int me) {
    extern volatile int panic_owner;
    int expected = -1;
    int claimed, status;
    __asm__ __volatile__(
        "1: ldxr  %w0, [%2]\n"
        "   cmp   %w0, %w3\n"
        "   b.ne  2f\n"
        "   stlxr %w1, %w4, [%2]\n"
        "   cbnz  %w1, 1b\n"
        "   mov   %w0, #1\n"
        "   b     3f\n"
        "2: mov   %w0, #0\n"
        "3:\n"
        : "=&r"(claimed), "=&r"(status)
        : "r"(&panic_owner), "r"(expected), "r"(me)
        : "memory", "cc");
    return claimed;
}

#endif
