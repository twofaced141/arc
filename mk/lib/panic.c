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


#include <stddef.h>
#include <stdint.h>
#include "panic.h"
#include "debug.h"
#include "scheduler.h"
#include "thread.h"
#include "cpu.h"
#ifndef __aarch64__
#include "terminal.h"
#endif

/* Panic ownership (SMP): exactly one CPU may write to the UART.
 * Set with a CAS — never a spinlock: a CPU panicking while another
 * holds a lock must not wait on it (deadlock).  -1 = nobody.
 * Non-static: panic.h's panic_claim() touches it directly. */
volatile int panic_owner = -1;

int panic_active(void) {
    return __atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE) != -1;
}

/* Interrupts off, wait forever.  Used by CPUs that lose the panic race
 * and by the owner after the dump. */
static void panic_halt(void) {
    for (;;) {
#if defined(__aarch64__)
        __asm__ __volatile__("msr daifset, #2" ::: "memory");
        __asm__ __volatile__("wfi");
#else
        __asm__ __volatile__("cli; hlt");
#endif
    }
}

/* Owner: quiet-stop every other online CPU (IPI_STOP), then wait until
 * they flip to CPU_OFFLINE so the dump below is the only UART traffic.
 * Bounded, short spin — it only needs to cover in-flight print/stop
 * delivery (~µs); a CPU stuck with IF=0 (e.g. mid-print) must not hold
 * the owner for long (TCG emulation makes large counts extremely slow). */
static void panic_stop_others(void) {
    struct cpu *self = cpu_current();

    unsigned sent = 0;
    for (unsigned i = 0; i < CPU_MAX; i++) {
        struct cpu *c = &cpus[i];
        if (c == self)
            continue;
        if (c->state != CPU_ONLINE)
            continue;           /* not up yet / already down: nothing to stop */
        cpu_send_ipi(c, IPI_STOP);
        sent++;
    }
    if (!sent)
        return;

    uint64_t spins = 0;
    while (spins++ < 200000ULL) {
        arch_cpu_relax();
        unsigned still_up = 0;
        for (unsigned i = 0; i < CPU_MAX; i++) {
            struct cpu *c = &cpus[i];
            if (c != self && c->state == CPU_ONLINE)
                still_up++;
        }
        if (still_up == 0)
            break;
    }
}

#if defined(__x86_64__)

#define P64_VA_MASK  0x000FFFFFFFFFF000ULL
#define P64_TEXT_LO  0x100000ULL
#define P64_TEXT_HI  0x117050ULL   /* end of .text + .rodata */

/* Is va present in the page tables addressed by cr3?
 * Page-table pages themselves live in the identity-mapped low
 * 64MB, so dereferencing them here is safe. */
static int amd64_va_mapped(uint64_t va, uint64_t cr3) {
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(cr3 & P64_VA_MASK);
    uint64_t e = pml4[(va >> 39) & 0x1FF];
    if (!(e & 1)) return 0;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(e & P64_VA_MASK);
    e = pdpt[(va >> 30) & 0x1FF];
    if (!(e & 1)) return 0;
    if (e & 0x80) return 1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(e & P64_VA_MASK);
    e = pd[(va >> 21) & 0x1FF];
    if (!(e & 1)) return 0;
    if (e & 0x80) return 1;
    uint64_t *pt = (uint64_t *)(uintptr_t)(e & P64_VA_MASK);
    return (pt[(va >> 12) & 0x1FF] & 1) != 0;
}

static const char *amd64_addr_tag(uint64_t a, uint64_t cr3) {
    if (a >= P64_TEXT_LO && a < P64_TEXT_HI)
        return "TEXT";
    if (a < 0x4000000ULL && amd64_va_mapped(a, cr3))
        return "map<64M";
    if (a >= 0xFFFFFFFF80000000ULL && amd64_va_mapped(a, cr3))
        return "kernel-hi";
    if (amd64_va_mapped(a, cr3))
        return "mapped";
    return "UNMAPPED";
}

/* Walk the RBP chain.  Everything is page-walk-guarded so the panic
 * handler itself cannot fault. */
static void amd64_backtrace(uint64_t rbp, uint64_t cr3) {
    uint64_t rbp_chain = rbp;
    debug_print_raw("Backtrace:\r\n");
    for (int i = 0; i < 24; i++) {
        if (!rbp_chain || rbp_chain < 0x1000 ||
            !amd64_va_mapped(rbp_chain, cr3) ||
            !amd64_va_mapped(rbp_chain + 8, cr3))
            break;
        uint64_t next = *(uint64_t *)(uintptr_t)rbp_chain;
        uint64_t ra = *(uint64_t *)(uintptr_t)(rbp_chain + 8);
        debug_printf_raw("  %02d: 0x%lx [%s]\r\n", i, ra, amd64_addr_tag(ra, cr3));
        if (next <= rbp_chain)
            break;
        rbp_chain = next;
    }
}

#endif /* __x86_64__ */

void panic(const char *reason, const registers_t *r) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("cli");
#endif

    /* Only the first CPU to panic prints; everyone else halts silently.
     * cpu_current() may be garbage if the fault corrupted GS — fall back
     * to cpu 0 so the owner still gets a dump. */
    struct cpu *self = cpu_current();
    int me = 0;
    if (self >= &cpus[0] && self < &cpus[CPU_MAX])
        me = (int)self->id;

    if (!panic_claim(me))
        panic_halt();           /* someone else owns the dump */

    /* Quiet the other CPUs first — the dump below must be the only
     * UART traffic. */
    panic_stop_others();

    debug_printf_raw("\r\npanic(cpu %u", me);
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
    debug_printf_raw(" caller 0x%lx", (unsigned long)__builtin_return_address(0));
#endif
    debug_printf_raw("): ");
    debug_print_raw(reason);
    debug_print_raw("\r\n");

    if (r) {
#if defined(__x86_64__)
        uint64_t cr2, cr3;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        debug_printf_raw("RIP: 0x%lx  RSP: 0x%lx  RBP: 0x%lx  CR2: 0x%lx\r\n",
                         r->rip, r->rsp, r->rbp, cr2);
        amd64_backtrace(r->rbp, cr3);
#elif defined(__aarch64__)
        debug_printf_raw("ELR: 0x%lx  SP: 0x%lx  LR: 0x%lx\r\n", r->elr, r->sp, r->lr);
#endif
    }

    debug_print_raw("System halted.\r\n");
    panic_halt();
}

void panic_simple(const char *reason) {
    panic(reason, NULL);
}
