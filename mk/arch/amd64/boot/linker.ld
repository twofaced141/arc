OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R+X: .text, .rodata */
    data PT_LOAD FLAGS(6);   /* R+W: .data, .bss */
}

SECTIONS {
    . = 1M;

    _kernel_start = .;

    .text : {
        *(.multiboot2)
        *(.text)
        *(.text.*)
    } :text

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :text

    .data : {
        *(.data)
        *(.data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    } :data

    /* AP trampoline — copied to 0x6000 by the SMP code at boot.  The
     * linked copy here is just storage; code runs from 0x6000. */
    .trampoline : {
        _trampoline_start = .;
        *(.trampoline)
        _trampoline_end = .;
    } :data

    _kernel_end = .;

    /DISCARD/ : {
        *(.note*)
        *(.eh_frame*)
        *(.got*)
        *(.comment)
    }
}
