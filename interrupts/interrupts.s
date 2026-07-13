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

/* Default IRQ stubs for vectors 48-255 */
.altmacro
.set _irq_n, 16
.set _vec_n, 48
.rept 208
    IRQ %_irq_n, %_vec_n
    .set _irq_n, _irq_n + 1
    .set _vec_n, _vec_n + 1
.endr
.noaltmacro

/* Table of default IRQ stub addresses (vectors 48-255) */
.section .rodata
.global irq_default_stubs
irq_default_stubs:
.include "irq_stubtable.inc"
.text

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
