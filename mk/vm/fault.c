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


#include "fault.h"
#include <stddef.h>
#include <stdint.h>
#include "thread.h"
#include "debug.h"
#include "personality.h"

int fault_try_recover(void) {
    thread_t *t = thread_current();
    if (!t || !t->fault_active || !t->fault_jb)
        return 0;
    fault_jmp_t *jb = t->fault_jb;
    /* Disarm before resuming: the recovery path returns -EFAULT
     * and must not itself be considered faultable. */
    t->fault_active = 0;
    t->fault_jb = NULL;
    fault_longjmp(jb);
    return 0; /* unreachable */
}

int fault_oops(uintptr_t fault_rip, uintptr_t fault_addr,
               const char *kind, int sig) {
    thread_t *t = thread_current();
    struct proc *p = NULL;

    if (t && proc_current)
        p = proc_current();

    if (!t || !p || !proc_exit) {
        /* No process to blame: idle thread, early boot, or a kernel
         * thread without BSD personality — the caller panics. */
        (void)fault_rip;
        (void)fault_addr;
        (void)kind;
        return 0;
    }

    /* NOTE: struct proc is opaque here (personality.h forward-declares
     * it so mk/ stays independent of bsd/proc.h) — tid identifies the
     * victim; proc_exit resolves the proc itself. */
    log_printf(LOG_LEVEL_ERROR,
               "oops: %s at 0x%lx addr 0x%lx tid=%u — killing process\r\n",
               kind,
               (unsigned long)fault_rip, (unsigned long)fault_addr,
               t->tid);

    /* A recycled thread slot must not inherit a stale window. */
    t->fault_active = 0;
    t->fault_jb = NULL;

    proc_exit(sig, NULL);
    thread_exit(sig);
    /* thread_exit switches away and never returns. */
    return 1;
}
