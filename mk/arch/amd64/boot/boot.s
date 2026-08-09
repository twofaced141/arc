.section .multiboot2
.balign 8
header_start:
    .long 0xE85250D6
    .long 0
    .long header_end - header_start
    .long 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))

    .balign 8
    .word 0
    .word 0
    .long 8
header_end:

.section .bss
.align 4096
pml4:
    .skip 4096
pdp:
    .skip 4096
pd:
    .skip 4096

stack_bottom:
    .skip 16384
stack_top:

.section .data
.align 8
mboot_magic:
    .quad 0
mboot_info:
    .quad 0

.section .rodata
.balign 8
gdt64:
    .quad 0
    .quad 0x0020980000000000
    .quad 0x0000920000000000
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
_start:
    cli

    /* Save multiboot parameters.
       VMA == LMA for all sections, so $symbol = physical address before paging. */
    movl %eax, mboot_magic
    movl %ebx, mboot_info

    movl $stack_top, %esp

    /* Check CPUID */
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    cmpl %ecx, %eax
    je .no_long_mode
    pushl %ecx
    popfl

    /* Check long mode via extended CPUID */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .no_long_mode

    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .no_long_mode

    /* PML4[0] = PDP phys | Present | Writable */
    movl $pdp, %eax
    orl $0x3, %eax
    movl %eax, (pml4)

    /* PML4[511] = same PDP (higher half) */
    movl $pdp, %eax
    orl $0x3, %eax
    movl %eax, (pml4 + 511 * 8)

    /* PDP[0] = PD phys | Present | Writable */
    movl $pd, %eax
    orl $0x3, %eax
    movl %eax, (pdp)

    /* PDP[510] = same PD (kernel at 0xFFFF_FFFF_80000000 via PML4[511]) */
    movl $pd, %eax
    orl $0x3, %eax
    movl %eax, (pdp + 510 * 8)

    /* PD entries: identity map first 16MB with 2MB pages
       (covers .text/.rodata/.data/.bss including stack) */
    movl $0x83, %eax
    movl $pd, %edi
    movl $16, %ecx
1:
    movl %eax, (%edi)
    addl $8, %edi
    addl $0x200000, %eax
    loop 1b

    /* Enable PAE */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    /* Load CR3 - VMA == LMA so $pml4 is both virtual and physical address */
    movl $pml4, %eax
    movl %eax, %cr3

    /* Enable long mode */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    /* Enable paging */
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    /* Load 64-bit GDT — after paging, operand is virtual; identity map covers it */
    lgdt (gdt64_ptr)

    /* Far jump to 64-bit mode */
    ljmp $0x08, $long_mode_start

.no_long_mode:
    cli
.halt:
    hlt
    jmp .halt

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ss
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    mov $stack_top, %rsp

    /* Restore multiboot params from temporary storage */
    mov (mboot_magic), %edi
    mov (mboot_info), %rsi

    call _entry

.hang:
    hlt
    jmp .hang
