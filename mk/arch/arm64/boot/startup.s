.section .data
.global boot_dtb_ptr
.align 8
boot_dtb_ptr:
    .quad 0
/* AP bringup globals — set by arch_cpu_start before PSCI CPU_ON.
 * Secondary entry (MMU off, identity-mapped) loads them via ldr =. */
.global ap_stack
.global ap_cpu
.global ap_ttbr
.align 8
ap_stack:
    .quad 0
ap_cpu:
    .quad 0
ap_ttbr:
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

/* PSCI secondary entry — entered via CPU_ON with x0 = context_id (struct cpu*).
 * MMU off, caches off, EL1.  Uses ap_* globals for stack/TTBR. */
.global arch_secondary_entry
.align 12
arch_secondary_entry:
    /* x0 = context_id == struct cpu* (passed via PSCI) */
    mov     x19, x0

    /* Set stack from ap_stack (set by BSP before PSCI call) */
    ldr     x1, =ap_stack
    ldr     x1, [x1]
    cbz     x1, .Lsec_hang
    mov     sp, x1

    /* VBAR + CPACR like BSP */
    ldr     x1, =vectors
    msr     vbar_el1, x1
    mov     x1, #0x300000
    msr     cpacr_el1, x1

    /* MAIR = 0xFF (MT_NORMAL WB, MT_DEVICE nGnRE) */
    mov     x1, #0xFF
    msr     mair_el1, x1

    /* TCR = 0x200003510 (48-bit, 4K, inner-shareable, WBWA, 40-bit IPS) */
    ldr     x1, =0x200003510
    msr     tcr_el1, x1

    /* TTBR0 = ap_ttbr (kernel_l1) */
    ldr     x1, =ap_ttbr
    ldr     x1, [x1]
    cbz     x1, .Lsec_hang
    msr     ttbr0_el1, x1
    dsb     ish
    isb

    /* Enable MMU + caches */
    mrs     x1, sctlr_el1
    orr     x1, x1, #0x1        /* SCTLR_MMU */
    orr     x1, x1, #0x4        /* SCTLR_CACHE */
    orr     x1, x1, #0x1000     /* SCTLR_I */
    orr     x1, x1, #0x10       /* SCTLR_SA0 */
    orr     x1, x1, #0x8        /* SCTLR_SA */
    msr     sctlr_el1, x1
    isb
    dsb     ish
    tlbi    vmalle1is
    dsb     ish
    isb

    /* Hand off to C: arch_ap_entry(cpu) */
    mov     x0, x19
    bl      arch_ap_entry

.Lsec_hang:
    wfi
    b       .Lsec_hang

.section .bss
.align 16
    .space 16384
stack_top:
