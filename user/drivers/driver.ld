ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);  /* PF_R */
    data   PT_LOAD FLAGS(6);  /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x401000;

    .text : {
        *(.text*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata*)
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data*)
        *(.bss*)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
    }
}
