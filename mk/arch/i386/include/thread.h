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
#include "vmm.h"
#include "isr.h"
#include "spinlock.h"

struct task;

#define THREAD_UNUSED   0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_ZOMBIE   4

#define MAX_THREADS       1024
#define THREAD_KSTACK_SIZE 8192
#define MAX_SLEEP_AVG 100
#define PRIO_MAX 140
#define MAX_RT_PRIO 100

struct prio_array;
typedef struct prio_array prio_array_t;

typedef struct thread {
    uint32_t tid;
    uint32_t state;
    uint32_t kernel_esp;
    uint8_t *kernel_stack;
    uint32_t kernel_stack_top;
    page_directory_t *page_dir;
    struct task *task;
    int32_t static_prio;
    int32_t prio;
    int32_t sleep_avg;
    uint32_t time_slice;
    struct thread *next;
    struct thread *prev;
    struct prio_array *array;
    uint32_t eip;
    uint32_t user_esp;
    uint32_t sleep_until;
    uintptr_t tls_base;          /* CLONE_SETTLS per-thread TLS base */
    char name[32];

    /* FPU/SSE state — FXSAVE area (x87 + XMM + MXCSR, 512 bytes).
     * 16-byte aligned as required by fxsave/fxrstor.  Switched lazily
     * via CR0.TS + #NM (see scheduler.c). */
    uint8_t fpu_state[512] __attribute__((aligned(16)));
} thread_t;

/* Initialise a 512-byte FXSAVE image to the architectural x87/SSE
 * reset state (FCW=0x037F, empty tag word, MXCSR=0x1F80). */
static inline void thread_fpu_state_init(uint8_t *st) {
    for (int i = 0; i < 512; i++) st[i] = 0;
    *(uint16_t *)&st[0]  = 0x037F;   /* FCW: RN, 64-bit precision, all exceptions masked */
    *(uint16_t *)&st[4]  = 0xFFFF;   /* FTW: all x87 registers empty */
    *(uint32_t *)&st[32] = 0x1F80;   /* MXCSR: RN, all SIMD exceptions masked */
}

void thread_init(void);
thread_t *thread_create(uint32_t eip, page_directory_t *page_dir, int user);
void thread_exit(int exitcode);
thread_t *thread_current(void);
thread_t *thread_find(uint32_t tid);
uint32_t thread_get_tid(void);
void thread_dump_all(void);

#endif
