.section .multiboot2
.balign 8
header_start:
    .long 0xE85250D6
    .long 0
    .long header_end - header_start
    .long 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))

    .align 8
    .word 0
    .word 0
    .long 8
header_end:

.set KERNEL_STACK, stack_top

.section .text
.global _start
_start:
    mov $stack_top, %esp
    mov %eax, %edi
    mov %ebx, %esi
    call gdt_install
    push %esi
    push %edi
    call kernel_main

.hang:
    hlt
    jmp .hang

.section .bss
.align 16
stack_bottom:
    .skip 65536
stack_top:
