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


#ifndef MK_PERSONALITY_H
#define MK_PERSONALITY_H

#include <stdint.h>
#include "isr.h"

/*
 * Optional BSD personality hooks.
 *
 * The microkernel builds and runs standalone.  The BSD personality
 * layer (~/Projects/bsd, built with `make WITH_BSD=1`) provides strong
 * definitions of these symbols; when it is not linked in, the weak
 * declarations below resolve to NULL and every call site in mk checks
 * for NULL before calling.
 */

struct proc;

extern void bsd_init(const char *cmdline) __attribute__((weak));
extern int64_t bsd_syscall_dispatch(registers_t *r) __attribute__((weak));
extern void sys_driver_irq_dispatch(uint8_t irq_num) __attribute__((weak));
extern struct proc *proc_current(void) __attribute__((weak));
extern void proc_exit(int code, registers_t *r) __attribute__((weak));

#endif /* MK_PERSONALITY_H */
