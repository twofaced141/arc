/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

#if defined(__x86_64__)
static inline void spin_lock_irqsave(spinlock_t *lock, uint32_t *flags) {
    uint64_t tmp;
    __asm__ __volatile__(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli\n\t"
        "1:\n\t"
        "movl $1, %%eax\n\t"
        "xchgl %%eax, %1\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1b\n\t"
        : "=r"(tmp), "=m"(*lock)
        :
        : "eax", "memory", "cc"
    );
    *flags = (uint32_t)tmp;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint32_t flags) {
    __asm__ __volatile__(
        "movl $0, %0\n\t"
        "pushq %1\n\t"
        "popfq\n\t"
        : "=m"(*lock)
        : "r"((uint64_t)flags)
        : "memory"
    );
}
#elif defined(__aarch64__)

static inline void spin_lock_irqsave(spinlock_t *lock, uint32_t *flags) {
    uint64_t daif;
    __asm__ __volatile__(
        "mrs %0, daif\n\t"
        "msr daifset, #2\n\t"          /* mask IRQ (same as x86 cli) */
        : "=&r"(daif)
        :
        : "memory");
    *flags = (uint32_t)daif;

    /* Atomic test-and-set acquire loop (same semantics as the x86
     * cli + lock xchgl path). */
    uint32_t old, tmp;
    __asm__ __volatile__(
        "1:\n\t"
        "ldaxr %w0, [%2]\n\t"          /* load-exclusive acquire */
        "cbnz %w0, 1b\n\t"             /* already held — retry */
        "mov %w1, #1\n\t"
        "stxr %w0, %w1, [%2]\n\t"      /* store-exclusive */
        "cbnz %w0, 1b\n\t"             /* lost the race — retry */
        : "=&r"(old), "=&r"(tmp)
        : "r"(lock)
        : "memory");
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint32_t flags) {
    __asm__ __volatile__(
        "stlr wzr, [%0]\n\t"           /* store-release zero */
        :
        : "r"(lock)
        : "memory");
    __asm__ __volatile__(
        "msr daif, %0\n\t"
        :
        : "r"((uint64_t)flags)
        : "memory");
}

#else
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

#endif
