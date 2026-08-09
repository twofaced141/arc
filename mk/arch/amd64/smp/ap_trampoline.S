/* x86_64 AP trampoline (Phase 5).
 *
 * The BSP copies this section (code + data fields) to TRAMPOLINE_BASE
 * (physical 0x6000) before sending INIT/SIPI.  The AP wakes in 16-bit
 * real mode at 0x6000, sets up paging/long mode using the BSP's CR3,
 * loads its per-CPU GS base, the shared GDT/IDT, and jumps to
 * arch_ap_entry().  Everything here is position-independent: data
 * fields are accessed through TRAMPOLINE_BASE-relative offsets that
 * are fixed by .org directives below (keep in sync with
 * mk/arch/amd64/smp/cpu.c).
 *
 * Field offsets (bytes):
 *   0x100 gdt_ptr   (2 limit + 8 base)
 *   0x10A idt_ptr   (2 limit + 8 base)
 *   0x118 cr3       (8)
 *   0x120 stack     (8)
 *   0x128 gs_base   (8)
 *   0x130 entry     (8)
 *   0x138 cpu       (8)
 */

.equ TRAMPOLINE_BASE, 0x6000

.section .trampoline, "awx"
.global _trampoline_start
_trampoline_start:

.code16
entry16:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $TRAMPOLINE_BASE, %sp

    /* runtime base into %bp — the AP always wakes at TRAMPOLINE_BASE
     * (SIPI vector = base >> 8), and the call/pop trick would yield a
     * CS-relative IP, not the absolute base. */
    movw $TRAMPOLINE_BASE, %bp

    /* CR4: PAE | OSFXSR | OSXMMEXCPT */
    movl %cr4, %eax
    orl  $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    movl %eax, %cr4

    /* CR3: BSP's kernel page tables (field 0x118) */
    movl 0x118(%bp), %eax
    movl %eax, %cr3

    /* EFER.LME = 1 */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* CR0: PE | PG */
    movl %cr0, %eax
    orl  $((1 << 0) | (1 << 31)), %eax
    movl %eax, %cr0

    /* The BIOS GDT has no valid code selector at 0x08 (the far jump
     * below needs one).  Load a minimal long-mode GDT built into the
     * trampoline; long64 switches to the kernel GDT afterwards. */
    lgdt (tramp_ap_gdt_ptr - _trampoline_start)(%bp)

    /* Far jump into 64-bit mode (selector 0x08 = long-mode code).
     * The 66 prefix makes the 16-bit EA read ptr16:32 — without it the
     * CPU would take the first 16 bits of the .long as the offset and
     * the next 16 bits (0x0000) as a NULL selector (#GP(0)). */
    .byte 0x66
    .byte 0xEA
    .long TRAMPOLINE_BASE + (long64 - _trampoline_start)
    .word 0x08

.code64
long64:
    movw $0x10, %ax
    movw %ax, %ss
    movw %ax, %ds
    movw %ax, %es

    movabsq $TRAMPOLINE_BASE, %rbx

    /* per-CPU kernel stack (field 0x120) */
    movq 0x120(%rbx), %rsp

    /* GS.base = &cpu->arch (field 0x128) */
    movl $0xC0000101, %ecx          /* IA32_GS_BASE */
    movq 0x128(%rbx), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    wrmsr

    /* Shared GDT/IDT (fields 0x100 / 0x10A) */
    lgdt 0x100(%rbx)
    lidt 0x10A(%rbx)

    /* arch_ap_entry(cpu) — fields 0x130 / 0x138 */
    movq 0x130(%rbx), %rax
    movq 0x138(%rbx), %rdi
    callq *%rax

ap_hang:
    hlt
    jmp ap_hang

/* Minimal GDT for the 16-bit protected-mode entry: null + code64 at
 * 0x08 + flat data at 0x10 (long64 loads SS/DS/ES = 0x10 before the
 * kernel GDT is switched in via field 0x100).  Filled by hand. */
.align 8
tramp_ap_gdt:
    .quad 0x0000000000000000        /* null */
    .quad 0x00AF9B000000FFFF        /* 0x08: long-mode code */
    .quad 0x00CF93000000FFFF        /* 0x10: flat data */
tramp_ap_gdt_ptr:
    .word (tramp_ap_gdt_ptr - tramp_ap_gdt - 1)
    .quad tramp_ap_gdt

/* Data fields at fixed offsets (see header comment) */
.org 0x100
.align 1
tramp_gdt_ptr:
    .word 0
    .quad 0
.org 0x10A
tramp_idt_ptr:
    .word 0
    .quad 0
.org 0x118
tramp_cr3:
    .quad 0
.org 0x120
tramp_stack:
    .quad 0
.org 0x128
tramp_gs_base:
    .quad 0
.org 0x130
tramp_entry:
    .quad 0
.org 0x138
tramp_cpu:
    .quad 0

.org 0x1000
.global _trampoline_end
_trampoline_end:
