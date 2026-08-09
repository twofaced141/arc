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


#ifndef BSD_ARCH_H
#define BSD_ARCH_H

#include <stdint.h>
#include "isr.h"
#include "thread.h"
#include "bsd/elf32.h"
#include "bsd/elf64.h"

#if defined(__x86_64__)
#  define BSD_ELF_CLASS   ELFCLASS64
#  define BSD_ELF_MACHINE EM_X86_64
#  define BSD_ELF_RELTYPE R_X86_64_RELATIVE
typedef uint64_t bsd_elf_addr_t;
#elif defined(__i386__) || defined(__i686__)
#  define BSD_ELF_CLASS   ELFCLASS32
#  define BSD_ELF_MACHINE EM_386
#  define BSD_ELF_RELTYPE R_386_RELATIVE
typedef uint32_t bsd_elf_addr_t;
#elif defined(__aarch64__)
#  ifndef EM_AARCH64
#    define EM_AARCH64 183
#  endif
#  ifndef R_AARCH64_RELATIVE
#    define R_AARCH64_RELATIVE 1027
#  endif
#  define BSD_ELF_CLASS   ELFCLASS64
#  define BSD_ELF_MACHINE EM_AARCH64
#  define BSD_ELF_RELTYPE R_AARCH64_RELATIVE
typedef uint64_t bsd_elf_addr_t;
#endif

#if defined(__x86_64__)
#  define bsd_syscall_num(r)    ((r)->rax - 1024)
#  define bsd_syscall_arg0(r)   ((r)->rdi)
#  define bsd_syscall_arg1(r)   ((r)->rsi)
#  define bsd_syscall_arg2(r)   ((r)->rdx)
#  define bsd_syscall_arg3(r)   ((r)->r10)
#  define bsd_syscall_arg4(r)   ((r)->r8)
#  define bsd_syscall_arg5(r)   ((r)->r9)
#  define bsd_syscall_ret(r,v)  ((r)->rax = (uint64_t)(v))
#  define bsd_syscall_ret0(r)   ((r)->rax = 0)
#  define bsd_entry(r)          ((r)->rip)
#  define BSD_SYSCALL_INS_LEN   2   /* `syscall` (0F 05) */
#elif defined(__i386__) || defined(__i686__)
#  define bsd_syscall_num(r)    ((r)->eax - 1024)
#  define bsd_syscall_arg0(r)   ((r)->ebx)
#  define bsd_syscall_arg1(r)   ((r)->ecx)
#  define bsd_syscall_arg2(r)   ((r)->edx)
#  define bsd_syscall_arg3(r)   ((r)->esi)
#  define bsd_syscall_arg4(r)   ((r)->edi)
#  define bsd_syscall_arg5(r)   ((r)->ebp)
#  define bsd_syscall_ret(r,v)  ((r)->eax = (uint32_t)(v))
#  define bsd_syscall_ret0(r)   ((r)->eax = 0)
#  define bsd_entry(r)          ((r)->eip)
#  define BSD_SYSCALL_INS_LEN   2   /* `int $0x80` (CD 80) */
#elif defined(__aarch64__)
#  define bsd_syscall_num(r)    ((r)->x[0] - 1024)
#  define bsd_syscall_arg0(r)   ((r)->x[1])
#  define bsd_syscall_arg1(r)   ((r)->x[2])
#  define bsd_syscall_arg2(r)   ((r)->x[3])
#  define bsd_syscall_arg3(r)   ((r)->x[4])
#  define bsd_syscall_arg4(r)   ((r)->x[5])
#  define bsd_syscall_arg5(r)   ((r)->x[6])
#  define bsd_syscall_ret(r,v)  ((r)->x[0] = (uint64_t)(v))
#  define bsd_syscall_ret0(r)   ((r)->x[0] = 0)
#  define bsd_entry(r)          ((r)->elr)
#  define BSD_SYSCALL_INS_LEN   4   /* `svc #0` (D4000001) */
#endif

void arch_setup_exec_regs(registers_t *r, uint64_t entry, uint64_t stack_top);
registers_t *arch_fork_setup_regs(thread_t *child_thread, registers_t *parent);
void arch_setup_tls_page(void *page, uint64_t tls_vaddr);
registers_t *arch_clone_setup_regs(thread_t *child_thread, registers_t *parent,
                                   uint64_t child_stack);
void arch_thread_set_tls(thread_t *t, uint64_t tls_base);

#endif