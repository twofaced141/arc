#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

static inline void spin_lock_irqsave(spinlock_t *lock, uint32_t *flags) {
    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli\n\t"
        "1:\n\t"
        "movl $1, %%eax\n\t"
        "xchgl %%eax, %1\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1b\n\t"
        : "=r"(*flags), "=m"(*lock)
        :
        : "eax", "memory", "cc"
    );
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint32_t flags) {
    __asm__ __volatile__(
        "movl $0, %0\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        : "=m"(*lock)
        : "r"(flags)
        : "memory"
    );
}

#endif
