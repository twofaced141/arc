/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * fault_jmp.S — setjmp/longjmp for recoverable kernel faults (amd64).
 *
 * Layout of fault_jmp_t.r[] (see mk/include/fault.h):
 *   r[0] rbx      r[1] rbp      r[2] r12      r[3] r13
 *   r[4] r14      r[5] r15      r[6] rsp      r[7] rip
 *   r[8] rflags
 */

.text
.code64
.globl fault_setjmp
.type fault_setjmp, @function
fault_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    pushfq
    popq %rax
    movq %rax, 64(%rdi)
    xorl %eax, %eax
    ret

.globl fault_longjmp
.type fault_longjmp, @function
fault_longjmp:
    movq 64(%rdi), %rax
    testq $0x200, %rax
    jz 1f
    sti
1:
    movq 56(%rdi), %rcx
    movq 48(%rdi), %rsp
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movl $1, %eax
    jmp *%rcx
