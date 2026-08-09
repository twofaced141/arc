.section .text

.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    push $\num
    jmp isr_common_stub
.endm

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_NOERRCODE 17
ISR_ERRCODE   18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31
ISR_NOERRCODE 128

.macro IRQ num, vec
.global irq\num
irq\num:
    push $0
    push $\vec
    jmp irq_common_stub
.endm

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

.extern isr_handler
isr_common_stub:
    pusha
    xor %eax, %eax
    mov %ds, %ax
    push %eax
    xor %eax, %eax
    mov %es, %ax
    push %eax
    xor %eax, %eax
    mov %fs, %ax
    push %eax
    xor %eax, %eax
    mov %gs, %ax
    push %eax
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    cld
    push %esp
    call isr_handler
    add $4, %esp
    push %esp
    call scheduler_switch
    add $4, %esp
    mov %eax, %esp
    pop %eax
    mov %ax, %gs
    pop %eax
    mov %ax, %fs
    pop %eax
    mov %ax, %es
    pop %eax
    mov %ax, %ds
    popa
    add $8, %esp
    iret

/* Builds a registers_t + iret frame so the thread resumes after the  */
/* call when the scheduler picks it again.                            */
/* ----------------------------------------------------------------- */
.global thread_yield
thread_yield:
    /* iret frame (CPU level) */
    push $0x10                              /* ss                     */
    lea 4(%esp), %eax                       /* eax = &ret_addr        */
    push %eax                               /* useresp                */
    pushf                                   /* eflags                 */
    push $0x08                              /* cs                     */
    lea .yield_resume, %eax
    push %eax                               /* eip                    */

    /* ISR level */
    push $0                                 /* err_code               */
    push $0                                 /* int_no                 */

    /* Register saves — same order as isr_common_stub */
    pushal                                  /* edi,esi,ebp,esp,ebx,edx,ecx,eax */

    /* Segment registers: ds, es, fs, gs (matching struct order) */
    xor %eax, %eax
    mov %ds, %ax
    push %eax                               /* ds                     */
    xor %eax, %eax
    mov %es, %ax
    push %eax                               /* es                     */
    xor %eax, %eax
    mov %fs, %ax
    push %eax                               /* fs                     */
    xor %eax, %eax
    mov %gs, %ax
    push %eax                               /* gs <- RSP              */

    push %esp
    call scheduler_switch
    add $4, %esp
    mov %eax, %esp                          /* switch to new stack    */

    /* Pop registers from (possibly new) thread's frame */
    pop %eax                                /* gs                     */
    mov %ax, %gs
    pop %eax                                /* fs                     */
    mov %ax, %fs
    pop %eax                                /* es                     */
    mov %ax, %es
    pop %eax                                /* ds                     */
    mov %ax, %ds
    popa                                    /* eax,ecx,edx,ebx,esp,ebp,esi,edi */

    add $8, %esp                            /* skip int_no, err_code  */
    iret

.yield_resume:
    /* For same-ring return (kernel→kernel), iret pops only EIP, CS,
     * EFLAGS (12 bytes), leaving the useresp- and ss-slots on the
     * stack.  Skip those 8 bytes so that `ret` reaches the caller's
     * return address that was at the top when thread_yield was
     * entered. */
    add $8, %esp
    ret

.extern irq_handler
irq_common_stub:
    pusha
    xor %eax, %eax
    mov %ds, %ax
    push %eax
    xor %eax, %eax
    mov %es, %ax
    push %eax
    xor %eax, %eax
    mov %fs, %ax
    push %eax
    xor %eax, %eax
    mov %gs, %ax
    push %eax
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    cld
    push %esp
    call irq_handler
    add $4, %esp
    push %esp
    call scheduler_switch
    add $4, %esp
    mov %eax, %esp
    pop %eax
    mov %ax, %gs
    pop %eax
    mov %ax, %fs
    pop %eax
    mov %ax, %es
    pop %eax
    mov %ax, %ds
    popa
    add $8, %esp
    iret
