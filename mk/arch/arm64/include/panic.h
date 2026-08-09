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
