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
#include "task.h"
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

#if defined(__i386__)

extern char _kernel_start[], _kernel_end[];

/* Is va present in the 2-level page tables addressed by cr3?  Page-table
 * pages themselves live in the identity-mapped low 1MB, so dereferencing
 * them here is safe.  With paging off (cr0.PG=0) the flat map covers
 * everything. */
static int i386_va_mapped(uint32_t va, uint32_t cr3, uint32_t cr0) {
    if (!(cr0 & (1u << 31)))
        return 1;
    uint32_t *pd = (uint32_t *)(uintptr_t)cr3;
    uint32_t e = pd[va >> 22];
    if (!(e & 1))
        return 0;
    if (e & 0x80)                /* 4MB page */
        return 1;
    uint32_t *pt = (uint32_t *)(uintptr_t)(e & ~0xFFFu);
    return (pt[(va >> 12) & 0x3FF] & 1) != 0;
}

static const char *i386_addr_tag(uint32_t a, uint32_t cr3, uint32_t cr0) {
    if (a >= (uint32_t)(uintptr_t)_kernel_start && a < (uint32_t)(uintptr_t)_kernel_end)
        return "TEXT";
    if (i386_va_mapped(a, cr3, cr0))
        return "mapped";
    return "UNMAPPED";
}

/* Walk the EBP chain.  Everything is page-walk-guarded so the panic
 * handler itself cannot fault. */
static void i386_backtrace(uint32_t ebp, uint32_t cr3, uint32_t cr0) {
    debug_print_raw("Backtrace:\r\n");
    uint32_t fp = ebp;
    for (int i = 0; i < 24; i++) {
        if (!fp || fp < 0x1000 ||
            !i386_va_mapped(fp, cr3, cr0) ||
            !i386_va_mapped(fp + 4, cr3, cr0))
            break;
        uint32_t next = *(uint32_t *)(uintptr_t)fp;
        uint32_t ra = *(uint32_t *)(uintptr_t)(fp + 4);
        debug_printf_raw("  %02d: 0x%08x [%s]\r\n", i, ra,
                         i386_addr_tag(ra, cr3, cr0));
        if (next <= fp)
            break;
        fp = next;
    }
}

#endif /* __i386__ */

#if defined(__aarch64__)

extern char _kernel_start[], _kernel_end[];

#define A64_RAM_BASE  0x40000000ULL
#define A64_RAM_SIZE  0x04000000ULL

/* The arm64 MMU identity-maps the first 64MB of RAM (kernel image and
 * all stacks live there) plus the UART/GIC; anything else is MMIO or
 * unmapped — refuse to dereference it in the panic handler. */
static int aarch64_addr_valid(uint64_t a) {
    return a >= A64_RAM_BASE && a < A64_RAM_BASE + A64_RAM_SIZE;
}

static const char *aarch64_addr_tag(uint64_t a) {
    if (a >= (uint64_t)(uintptr_t)_kernel_start && a < (uint64_t)(uintptr_t)_kernel_end)
        return "TEXT";
    if (aarch64_addr_valid(a))
        return "mapped";
    return "UNMAPPED";
}

/* Walk the X29 (frame pointer) chain: [fp] = previous fp, [fp+8] = LR. */
static void aarch64_backtrace(uint64_t fp) {
    debug_print_raw("Backtrace:\r\n");
    for (int i = 0; i < 24; i++) {
        if (!fp || !aarch64_addr_valid(fp) || !aarch64_addr_valid(fp + 8))
            break;
        uint64_t next = *(uint64_t *)(uintptr_t)fp;
        uint64_t ra = *(uint64_t *)(uintptr_t)(fp + 8);
        debug_printf_raw("  %02d: 0x%016lx [%s]\r\n", i, ra, aarch64_addr_tag(ra));
        if (next <= fp)
            break;
        fp = next;
    }
}

#endif /* __aarch64__ */

static const char *thread_state_str(uint32_t state) {
    switch (state) {
    case THREAD_UNUSED:  return "unused";
    case THREAD_READY:   return "ready";
    case THREAD_RUNNING: return "running";
    case THREAD_BLOCKED: return "blocked";
    case THREAD_ZOMBIE:  return "zombie";
    default:             return "unknown";
    }
}

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

    thread_t *t = thread_current();
    if (t) {
        debug_printf_raw("Thread: %s (tid=%u, pid=%u) state=%s\r\n",
                         t->name[0] ? t->name : "unnamed",
                         t->tid,
                         t->task ? t->task->task_id : 0,
                         thread_state_str(t->state));
    }

    if (r) {
#if defined(__x86_64__)
        uint64_t cr0, cr2, cr3, cr4;
        __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
        debug_printf_raw("RAX: 0x%016lx  RBX: 0x%016lx\r\n", r->rax, r->rbx);
        debug_printf_raw("RCX: 0x%016lx  RDX: 0x%016lx\r\n", r->rcx, r->rdx);
        debug_printf_raw("RSI: 0x%016lx  RDI: 0x%016lx\r\n", r->rsi, r->rdi);
        debug_printf_raw("RBP: 0x%016lx  RSP: 0x%016lx\r\n", r->rbp, r->rsp);
        debug_printf_raw("R8:  0x%016lx  R9:  0x%016lx\r\n", r->r8, r->r9);
        debug_printf_raw("R10: 0x%016lx  R11: 0x%016lx\r\n", r->r10, r->r11);
        debug_printf_raw("R12: 0x%016lx  R13: 0x%016lx\r\n", r->r12, r->r13);
        debug_printf_raw("R14: 0x%016lx  R15: 0x%016lx\r\n", r->r14, r->r15);
        debug_printf_raw("RIP: 0x%016lx  CS: 0x%016lx\r\n", r->rip, r->cs);
        debug_printf_raw("RFLAGS: 0x%016lx  SS: 0x%016lx\r\n", r->rflags, r->ss);
        debug_printf_raw("VEC: 0x%016lx  ERR: 0x%016lx\r\n", r->int_no, r->err_code);
        debug_printf_raw("CR0: 0x%016lx  CR2: 0x%016lx\r\n", cr0, cr2);
        debug_printf_raw("CR3: 0x%016lx  CR4: 0x%016lx\r\n", cr3, cr4);
        amd64_backtrace(r->rbp, cr3);
#elif defined(__i386__)
        debug_printf_raw("EAX: 0x%08x  EBX: 0x%08x\r\n", r->eax, r->ebx);
        debug_printf_raw("ECX: 0x%08x  EDX: 0x%08x\r\n", r->ecx, r->edx);
        debug_printf_raw("ESI: 0x%08x  EDI: 0x%08x\r\n", r->esi, r->edi);
        debug_printf_raw("EBP: 0x%08x  ESP: 0x%08x\r\n", r->ebp, r->esp);
        debug_printf_raw("EIP: 0x%08x  CS: 0x%08x\r\n", r->eip, r->cs);
        debug_printf_raw("EFLAGS: 0x%08x  SS: 0x%08x\r\n", r->eflags, r->ss);
        debug_printf_raw("USERESP: 0x%08x  GS: 0x%08x\r\n", r->useresp, r->gs);
        debug_printf_raw("FS: 0x%08x  ES: 0x%08x\r\n", r->fs, r->es);
        debug_printf_raw("DS: 0x%08x\r\n", r->ds);
        debug_printf_raw("VEC: 0x%08x  ERR: 0x%08x\r\n", r->int_no, r->err_code);
        uint32_t cr0, cr2, cr3;
        __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        debug_printf_raw("CR0: 0x%08x  CR2: 0x%08x\r\n", cr0, cr2);
        debug_printf_raw("CR3: 0x%08x\r\n", cr3);
        i386_backtrace(r->ebp, cr3, cr0);
#elif defined(__aarch64__)
        for (int i = 0; i < 30; i += 4) {
            debug_printf_raw("X%02d: 0x%016lx  X%02d: 0x%016lx\r\n",
                             i, r->x[i], i + 1, r->x[i + 1]);
        }
        debug_printf_raw("X30: 0x%016lx  SPSR: 0x%016lx\r\n", r->lr, r->spsr);
        debug_printf_raw("ELR: 0x%016lx  ESR: 0x%016lx\r\n", r->elr, r->esr);
        debug_printf_raw("FAR: 0x%016lx  SP: 0x%016lx\r\n", r->far, r->sp);
        aarch64_backtrace(r->x[29]);
#endif
    }

    debug_print_raw("System halted.\r\n");
    panic_halt();
}

void panic_simple(const char *reason) {
    panic(reason, NULL);
}
