.section .data
.global boot_dtb_ptr
.align 8
boot_dtb_ptr:
    .quad 0

.section .text._start
.global _start
_start:
    /* Save DTB address from x0 (QEMU boot protocol: x0 = DTB phys addr).
     * Must save before clobbering x0.  Using a .data variable
     * because x0 may be 0 for ELF -kernel boots, but we store it anyway. */
    ldr     x1, =boot_dtb_ptr
    str     x0, [x1]

    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, .Lsecondary

    ldr     x0, =stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    cbz     x1, .Lcall_main
    mov     x2, xzr
.Lbss_loop:
    str     x2, [x0], #8
    subs    x1, x1, #8
    bne     .Lbss_loop

    ldr     x0, =vectors
    msr     vbar_el1, x0

    mov     x0, #0x300000
    msr     cpacr_el1, x0

.Lcall_main:
    /* x0 = DTB phys addr (QEMU raw Image protocol); kernel_main_fdt
     * turns it into an arc_boot_info and calls kernel_main(boot). */
    bl      kernel_main_fdt

.Lhalt:
    wfi
    b       .Lhalt

.Lsecondary:
    wfi
    b       .Lsecondary

.section .bss
.align 16
    .space 16384
stack_top:
