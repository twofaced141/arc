.section .text.vectors
.align 12
.global vectors
vectors:
    .org vectors + 0x000
    b sync_el1

    .org vectors + 0x080
    b irq_entry

    .org vectors + 0x100
    b fiq_panic

    .org vectors + 0x180
    b serr_panic

    .org vectors + 0x200
    b sync_el1

    .org vectors + 0x280
    b irq_entry

    .org vectors + 0x300
    b fiq_panic

    .org vectors + 0x380
    b serr_panic

    .org vectors + 0x400
    b sync_lower

    .org vectors + 0x480
    b irq_entry

    .org vectors + 0x500
    b fiq_panic

    .org vectors + 0x580
    b serr_panic

    .org vectors + 0x600
    b sync_panic

    .org vectors + 0x680
    b fiq_panic

    .org vectors + 0x700
    b fiq_panic

    .org vectors + 0x780
    b serr_panic

    .org vectors + 0x800

.section .text
.global exception_entry
.global irq_entry
.global thread_exit_switch

/* Switch to a scheduler-chosen thread's frame and eret.
 * Called from thread_exit(): x0 = next thread's kernel_rsp.
 * Restores the full frame (same layout as the exception entry) and
 * returns to the thread where it left off. */
thread_exit_switch:
    mov sp, x0
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret

irq_entry:
    sub sp, sp, #288
    stp x0, x1, [sp, #0*16]
    stp x2, x3, [sp, #1*16]
    stp x4, x5, [sp, #2*16]
    stp x6, x7, [sp, #3*16]
    stp x8, x9, [sp, #4*16]
    stp x10, x11, [sp, #5*16]
    stp x12, x13, [sp, #6*16]
    stp x14, x15, [sp, #7*16]
    stp x16, x17, [sp, #8*16]
    stp x18, x19, [sp, #9*16]
    stp x20, x21, [sp, #10*16]
    stp x22, x23, [sp, #11*16]
    stp x24, x25, [sp, #12*16]
    stp x26, x27, [sp, #13*16]
    stp x28, x29, [sp, #14*16]
    str x30, [sp, #240]
    mrs x0, spsr_el1
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
    mrs x0, sp_el0
    str x0, [sp, #280]
    mov x0, sp
    bl irq_handler
    mov sp, x0
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret

sync_el1:
    sub sp, sp, #288
    stp x0, x1, [sp, #0*16]
    stp x2, x3, [sp, #1*16]
    stp x4, x5, [sp, #2*16]
    stp x6, x7, [sp, #3*16]
    stp x8, x9, [sp, #4*16]
    stp x10, x11, [sp, #5*16]
    stp x12, x13, [sp, #6*16]
    stp x14, x15, [sp, #7*16]
    stp x16, x17, [sp, #8*16]
    stp x18, x19, [sp, #9*16]
    stp x20, x21, [sp, #10*16]
    stp x22, x23, [sp, #11*16]
    stp x24, x25, [sp, #12*16]
    stp x26, x27, [sp, #13*16]
    stp x28, x29, [sp, #14*16]
    str x30, [sp, #240]
    mrs x0, spsr_el1
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
    mrs x0, sp_el0
    str x0, [sp, #280]
    mov x0, sp
    bl sync_handler
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret

sync_lower:
    sub sp, sp, #288
    stp x0, x1, [sp, #0*16]
    stp x2, x3, [sp, #1*16]
    stp x4, x5, [sp, #2*16]
    stp x6, x7, [sp, #3*16]
    stp x8, x9, [sp, #4*16]
    stp x10, x11, [sp, #5*16]
    stp x12, x13, [sp, #6*16]
    stp x14, x15, [sp, #7*16]
    stp x16, x17, [sp, #8*16]
    stp x18, x19, [sp, #9*16]
    stp x20, x21, [sp, #10*16]
    stp x22, x23, [sp, #11*16]
    stp x24, x25, [sp, #12*16]
    stp x26, x27, [sp, #13*16]
    stp x28, x29, [sp, #14*16]
    str x30, [sp, #240]
    mrs x0, spsr_el1
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
    mrs x0, sp_el0
    str x0, [sp, #280]
    mov x0, sp
    bl sync_handler
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret

fiq_panic:
    sub sp, sp, #288
    stp x0, x1, [sp, #0*16]
    stp x2, x3, [sp, #1*16]
    stp x4, x5, [sp, #2*16]
    stp x6, x7, [sp, #3*16]
    stp x8, x9, [sp, #4*16]
    stp x10, x11, [sp, #5*16]
    stp x12, x13, [sp, #6*16]
    stp x14, x15, [sp, #7*16]
    stp x16, x17, [sp, #8*16]
    stp x18, x19, [sp, #9*16]
    stp x20, x21, [sp, #10*16]
    stp x22, x23, [sp, #11*16]
    stp x24, x25, [sp, #12*16]
    stp x26, x27, [sp, #13*16]
    stp x28, x29, [sp, #14*16]
    str x30, [sp, #240]
    mrs x0, spsr_el1
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
    mrs x0, sp_el0
    str x0, [sp, #280]
    mov x0, sp
    bl fiq_handler
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret

serr_panic:
    sub sp, sp, #288
    stp x0, x1, [sp, #0*16]
    stp x2, x3, [sp, #1*16]
    stp x4, x5, [sp, #2*16]
    stp x6, x7, [sp, #3*16]
    stp x8, x9, [sp, #4*16]
    stp x10, x11, [sp, #5*16]
    stp x12, x13, [sp, #6*16]
    stp x14, x15, [sp, #7*16]
    stp x16, x17, [sp, #8*16]
    stp x18, x19, [sp, #9*16]
    stp x20, x21, [sp, #10*16]
    stp x22, x23, [sp, #11*16]
    stp x24, x25, [sp, #12*16]
    stp x26, x27, [sp, #13*16]
    stp x28, x29, [sp, #14*16]
    str x30, [sp, #240]
    mrs x0, spsr_el1
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
    mrs x0, sp_el0
    str x0, [sp, #280]
    mov x0, sp
    bl serr_handler
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret

sync_panic:
    sub sp, sp, #288
    stp x0, x1, [sp, #0*16]
    stp x2, x3, [sp, #1*16]
    stp x4, x5, [sp, #2*16]
    stp x6, x7, [sp, #3*16]
    stp x8, x9, [sp, #4*16]
    stp x10, x11, [sp, #5*16]
    stp x12, x13, [sp, #6*16]
    stp x14, x15, [sp, #7*16]
    stp x16, x17, [sp, #8*16]
    stp x18, x19, [sp, #9*16]
    stp x20, x21, [sp, #10*16]
    stp x22, x23, [sp, #11*16]
    stp x24, x25, [sp, #12*16]
    stp x26, x27, [sp, #13*16]
    stp x28, x29, [sp, #14*16]
    str x30, [sp, #240]
    mrs x0, spsr_el1
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
    mrs x0, sp_el0
    str x0, [sp, #280]
    mov x0, sp
    bl sync_handler
    ldr x0, [sp, #280]
    msr sp_el0, x0
    ldr x0, [sp, #272]
    msr far_el1, x0
    ldr x0, [sp, #264]
    msr esr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #248]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #14*16]
    ldp x26, x27, [sp, #13*16]
    ldp x24, x25, [sp, #12*16]
    ldp x22, x23, [sp, #11*16]
    ldp x20, x21, [sp, #10*16]
    ldp x18, x19, [sp, #9*16]
    ldp x16, x17, [sp, #8*16]
    ldp x14, x15, [sp, #7*16]
    ldp x12, x13, [sp, #6*16]
    ldp x10, x11, [sp, #5*16]
    ldp x8, x9, [sp, #4*16]
    ldp x6, x7, [sp, #3*16]
    ldp x4, x5, [sp, #2*16]
    ldp x2, x3, [sp, #1*16]
    ldp x0, x1, [sp, #0*16]
    add sp, sp, #288
    eret
