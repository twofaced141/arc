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


#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include "isr.h"

struct task;

#define THREAD_UNUSED   0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_ZOMBIE   4

#define MAX_THREADS     1024
#define THREAD_KSTACK_SIZE  8192
#define MAX_SLEEP_AVG   100
#define PRIO_MAX        140
#define MAX_RT_PRIO     100

typedef struct prio_array prio_array_t;

typedef struct thread {
    uint32_t tid;
    uint32_t state;
    uint64_t kernel_rsp;
    uint8_t *kernel_stack;
    uint64_t kernel_stack_top;
    void *page_dir;
    struct task *task;
    int32_t static_prio;
    int32_t prio;
    int32_t sleep_avg;
    uint32_t time_slice;
    struct thread *next;
    struct thread *prev;
    prio_array_t *array;
    uint64_t entry;
    uint32_t sleep_until;
    uint64_t tls_base;              /* per-thread TLS base (unused on arm64 yet) */
    char name[32];

    /* FP/SIMD state — Q0-Q31 (each 16 bytes, offsets 0..496), then
     * FPSR (512) and FPCR (516).  16-byte aligned for stp/ldp q.     *
     * Switched eagerly in the scheduler (no lazy TS mechanism on     *
     * AArch64 EL1).                                                   */
    uint8_t fpu_state[576] __attribute__((aligned(16)));
} thread_t;

/* Initialise the FP/SIMD state image to a clean reset state. */
static inline void thread_fpu_state_init(uint8_t *st) {
    for (int i = 0; i < 576; i++) st[i] = 0;
    *(uint32_t *)&st[516] = 0x00000000;   /* FPCR: round-to-nearest, no traps */
    *(uint32_t *)&st[512] = 0x00000000;   /* FPSR */
}

void thread_init(void);
thread_t *thread_create(uint64_t entry, void *page_dir, int user);
void thread_exit(int exitcode);
thread_t *thread_current(void);
uint32_t thread_get_tid(void);
thread_t *thread_find(uint32_t tid);

#endif
