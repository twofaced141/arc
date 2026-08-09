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


/* sigexec — exec-time signal state probe.
 *
 * Kills itself with SIGUSR1 and exits.  The parent (init) sets up the
 * signal disposition BEFORE exec and decides from the exit status
 * whether exec reset the state correctly:
 *   - caught handler must revert to SIG_DFL  → dies by SIGUSR1
 *   - SIG_IGN must survive                  → survives, exits 0
 */

#include "syscall.h"

static unsigned long my_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned long)(p - s);
}

static long bsd_write(int fd, const void *buf, unsigned long cnt) {
    return syscall3(BSD_SYS(SYS_WRITE), fd, (long)buf, cnt);
}
static long bsd_exit(long code) {
    return syscall1(BSD_SYS(SYS_EXIT), code);
}
static long bsd_getpid(void) {
    return syscall0(BSD_SYS(SYS_GETPID));
}
static long bsd_kill(long pid, int sig) {
    return syscall2(BSD_SYS(SYS_KILL), pid, sig);
}

static void print(const char *s) {
    bsd_write(1, s, my_strlen(s));
}

static void print_dec(long v) {
    char buf[20];
    int i = 20;
    if (v < 0) {
        bsd_write(1, "-", 1);
        v = -v;
    }
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);
    bsd_write(1, buf + i, 20 - i);
}

__attribute__((noreturn))
void _start(void) {
    print("sigexec: pid=");
    print_dec(bsd_getpid());
    print(" killing self with SIGUSR1\n");

    bsd_kill(bsd_getpid(), 10 /* SIGUSR1 */);
    bsd_exit(0);
    __builtin_unreachable();
}
