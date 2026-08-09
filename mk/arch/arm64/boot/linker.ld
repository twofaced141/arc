OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

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
