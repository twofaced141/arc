/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * fault_jmp.S — setjmp/longjmp for recoverable kernel faults (i386).
 *
 * Layout of fault_jmp_t.r[] (see mk/include/fault.h):
 *   r[0] ebx  r[1] esi  r[2] edi  r[3] ebp  r[4] esp  r[5] eip
 *   r[6] eflags
 */

.text
.code32
.globl fault_setjmp
.type fault_setjmp, @function
fault_setjmp:
    movl 4(%esp), %eax
    movl %ebx, 0(%eax)
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 16(%eax)
    movl (%esp), %ecx
    movl %ecx, 20(%eax)
    pushfl
    popl %ecx
    movl %ecx, 24(%eax)
    xorl %eax, %eax
    ret

.globl fault_longjmp
.type fault_longjmp, @function
fault_longjmp:
    movl 4(%esp), %eax
    movl 24(%eax), %ecx
    testl $0x200, %ecx
    jz 1f
    sti
1:
    movl 20(%eax), %ecx
    movl 16(%eax), %esp
    movl 0(%eax), %ebx
    movl 4(%eax), %esi
    movl 8(%eax), %edi
    movl 12(%eax), %ebp
    movl $1, %eax
    jmp *%ecx
