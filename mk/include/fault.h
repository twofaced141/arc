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


#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

/*
 * fault.h — recoverable kernel faults (minimum ring0 isolation).
 *
 * Model: setjmp-style recovery points.  Code that touches user memory
 * (copy_from/to_user, strncpy_from_user) saves a recovery context on
 * its own stack, publishes it in the current thread
 * (fault_active + fault_jb), and calls fault_setjmp().  A kernel
 * #PF/#GP/Data-Abort that fires while the context is armed is
 * redirected by fault_try_recover() back into the copier, which
 * observes fault_setjmp() returning 1 and fails the syscall with
 * -EFAULT — instead of panicking the machine.
 *
 * Why setjmp and not a RIP window: label addresses (&&label) are at
 * the mercy of optimizer block layout and cross-jumping — the fail
 * block was observed merged into the success path.  setjmp/longjmp
 * with returns_twice is optimizer-proof by construction.
 *
 * Anything that faults OUTSIDE an armed window in process context
 * becomes an oops (kill the process via proc_exit + thread_exit);
 * faults without a process (idle, early boot, kernel threads) still
 * panic.  fault_jmp restores sp/fp/callee-saved regs/pc only — the
 * recovery path must return immediately without touching locals.
 */

#define FAULT_JMP_SLOTS 16

typedef struct fault_jmp {
    uintptr_t r[FAULT_JMP_SLOTS];
} fault_jmp_t;

/* Save callee-saved regs + sp + pc (+ interrupt flag on x86).
 * Returns 0 on the direct call, 1 when resumed via fault_longjmp.
 * Arch asm (mk/arch/<arch>/thread/fault_jmp.S). */
int fault_setjmp(fault_jmp_t *j) __attribute__((returns_twice));

/* Restore a context saved by fault_setjmp (fault_setjmp then
 * returns 1).  Restores the saved interrupt state on x86.  Arch asm. */
void fault_longjmp(fault_jmp_t *j) __attribute__((noreturn));

/* Called from kernel fault handlers: if the current thread has an
 * armed recovery context, disarm it and longjmp into it (does not
 * return).  Returns 0 when nothing is armed. */
int fault_try_recover(void);

/* Non-recoverable kernel fault policy: log an oops and, when the
 * fault happened in process context (proc_current() != NULL), kill
 * the process and its thread (does not return).  Returns 0 when
 * there is no process to blame — the caller must panic().
 * sig is the exit code used for proc_exit/thread_exit (139 for #PF,
 * 134 for #GP/#UD, ...). */
int fault_oops(uintptr_t fault_rip, uintptr_t fault_addr,
               const char *kind, int sig);

#endif /* FAULT_H */
