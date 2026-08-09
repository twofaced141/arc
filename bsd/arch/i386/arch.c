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


#include "bsd/arch.h"
#include "vmm.h"
#include "string.h"

#ifdef __i386__
void arch_setup_exec_regs(registers_t *r, uint64_t entry, uint64_t stack_top) {
    r->gs = 0x23; r->fs = 0x23; r->es = 0x23; r->ds = 0x23;
    r->edi = 0; r->esi = 0; r->ebp = 0;
    r->esp = (uint32_t)(uintptr_t)&r->int_no;
    r->ebx = 0; r->edx = 0; r->ecx = 0; r->eax = 0;
    r->int_no = 0; r->err_code = 0;
    r->eip = (uint32_t)entry;
    r->cs = 0x1B;
    r->eflags = 0x202;
    r->useresp = (uint32_t)stack_top;
    r->ss = 0x23;
}

registers_t *arch_fork_setup_regs(thread_t *child_thread, registers_t *parent) {
    registers_t *child_regs = (registers_t *)(child_thread->kernel_stack_top - sizeof(registers_t));
    memcpy(child_regs, parent, sizeof(registers_t));
    child_regs->eax = 0;
    child_thread->kernel_esp = (uint32_t)(uintptr_t)child_regs;
    child_thread->user_esp = parent->useresp;
    child_thread->eip = parent->eip;
    return child_regs;
}

registers_t *arch_clone_setup_regs(thread_t *child_thread, registers_t *parent,
                                   uint64_t child_stack) {
    registers_t *child_regs = (registers_t *)(child_thread->kernel_stack_top - sizeof(registers_t));
    memcpy(child_regs, parent, sizeof(registers_t));
    child_regs->eax = 0;
    child_regs->esp = (uint32_t)child_stack;
    child_thread->kernel_esp = (uint32_t)(uintptr_t)child_regs;
    child_thread->user_esp = (uint32_t)child_stack;
    child_thread->eip = parent->eip;
    return child_regs;
}

void arch_thread_set_tls(thread_t *t, uint64_t tls_base) {
    t->tls_base = (uintptr_t)tls_base;
}

void arch_setup_tls_page(void *page, uint64_t tls_vaddr) {
    uint8_t *p = (uint8_t *)page;
    uint32_t tls_self = (uint32_t)tls_vaddr;
    p[0] = tls_self & 0xFF; p[1] = (tls_self >> 8) & 0xFF;
    p[2] = (tls_self >> 16) & 0xFF; p[3] = (tls_self >> 24) & 0xFF;
    p[8] = tls_self & 0xFF; p[9] = (tls_self >> 8) & 0xFF;
    p[10] = (tls_self >> 16) & 0xFF; p[11] = (tls_self >> 24) & 0xFF;
    uint32_t sysinfo = tls_self + 0x100;
    p[0x10] = sysinfo & 0xFF; p[0x11] = (sysinfo >> 8) & 0xFF;
    p[0x12] = (sysinfo >> 16) & 0xFF; p[0x13] = (sysinfo >> 24) & 0xFF;
    p[0x14] = 0xDE; p[0x15] = 0xAD; p[0x16] = 0xBE; p[0x17] = 0xEF;
    p[0x100] = 0xCD; p[0x101] = 0x80; p[0x102] = 0xC3;
}
#endif
