OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data   PT_LOAD FLAGS(7);   /* PF_R | PF_W | PF_X */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text.start)
        *(.text .text.*)
        *(.ltext .ltext.*)
        *(.rodata .rodata.*)
        *(.lrodata .lrodata.*)
    } :text

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.lbss .lbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note.*)
        *(.rela.*)
        *(.dynamic)
        *(.dynsym)
    }
}
