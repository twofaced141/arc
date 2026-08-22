OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* Default load address: RAM_BASE (DT) + 0x80000 (Linux arm64 convention).
 * QEMU virt default RAM 0x40000000 -> 0x40080000.  For a different board
 * the bootloader must load us at this address or the kernel must be made
 * position-independent — physical addresses themselves are DT-driven via
 * platform.h after this point. */
SECTIONS {
    . = 0x40080000;
    _kernel_start = .;

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
    }

    .rodata : { *(.rodata) *(.rodata.*) }

    .data : { *(.data) *(.data.*) }

    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
    __bss_end = .;

    . = ALIGN(16);
    __heap_start = .;
    . += 8M;
    __heap_end = .;

    _kernel_end = .;

    /DISCARD/ : { *(.note*) *(.eh_frame*) *(.comment) }

}
