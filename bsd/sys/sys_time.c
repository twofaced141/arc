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


#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/time.h"
#include "bsd/arch.h"
#include "vmm.h"
#include "clockevent.h"

#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))

/* System time base: monotonic seconds derived from the PIT tick
 * counter (100 Hz).  There is no RTC-backed wall clock yet, so
 * CLOCK_REALTIME == CLOCK_MONOTONIC. */
static uint64_t sys_now_secs(void) {
    return clockevent_get_ticks() / 100;
}

int64_t sys_gettimeofday(proc_t *p, registers_t *r) {
    (void)p;
    struct timeval *tv = (struct timeval *)ARG1(r);
    if (!tv)
        return 0;   /* NULL tz argument also allowed; ignore both */

    struct timeval ktv;
    uint64_t secs = sys_now_secs();
    ktv.tv_sec = (int64_t)secs;
    ktv.tv_usec = (long)((clockevent_get_ticks() % 100) * 10000);
    if (copy_to_user(tv, &ktv, sizeof(ktv)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_clock_gettime(proc_t *p, registers_t *r) {
    (void)p;
    int clk = (int)ARG1(r);
    struct timespec *tp = (struct timespec *)ARG2(r);
    if (!tp)
        return -EFAULT;
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)
        return -EINVAL;

    struct timespec kts;
    uint64_t secs = sys_now_secs();
    kts.tv_sec = (int64_t)secs;
    kts.tv_nsec = (long)((clockevent_get_ticks() % 100) * 10000000);
    if (copy_to_user(tp, &kts, sizeof(kts)) < 0)
        return -EFAULT;
    return 0;
}
