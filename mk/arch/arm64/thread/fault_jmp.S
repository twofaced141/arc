/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * fault_jmp.S — setjmp/longjmp for recoverable kernel faults (arm64).
 *
 * Layout of fault_jmp_t.r[] (see mk/include/fault.h), byte offsets:
 *   r[0..1]   sp, lr     r[2..3]   x29, x19   r[4..5]   x20, x21
 *   r[6..7]   x22, x23   r[8..9]   x24, x25   r[10..11] x26, x27
 *   r[12]     x28
 *
 * Sync exceptions do not alter DAIF masking, so no flag save/restore
 * is needed (unlike x86, where interrupt gates clear IF).
 */

.text
.align 2
.globl fault_setjmp
.type fault_setjmp, @function
fault_setjmp:
    mov x1, sp
    stp x1, x30, [x0, #0]
    stp x29, x19, [x0, #16]
    stp x20, x21, [x0, #32]
    stp x22, x23, [x0, #48]
    stp x24, x25, [x0, #64]
    stp x26, x27, [x0, #80]
    str x28, [x0, #96]
    mov x0, #0
    ret

.globl fault_longjmp
.type fault_longjmp, @function
fault_longjmp:
    ldp x1, x30, [x0, #0]
    mov sp, x1
    ldp x29, x19, [x0, #16]
    ldp x20, x21, [x0, #32]
    ldp x22, x23, [x0, #48]
    ldp x24, x25, [x0, #64]
    ldp x26, x27, [x0, #80]
    ldr x28, [x0, #96]
    mov x0, #1
    ret
