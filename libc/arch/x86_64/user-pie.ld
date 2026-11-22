OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD FILEHDR PHDRS FLAGS(5);   /* PF_R | PF_X */
    data    PT_LOAD FLAGS(6);                 /* PF_R | PF_W */
    dynamic PT_DYNAMIC FLAGS(6);              /* PF_R | PF_W */
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .text : {
        *(.text.start)
        *(.text .text.*)
        *(.ltext .ltext.*)
        *(.rodata .rodata.*)
        *(.lrodata .lrodata.*)
    } :text

    . = ALIGN(4096);

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    .got : {
        *(.got .got.*)
    } :data

    .got.plt : {
        *(.got.plt)
    } :data

    .rela.dyn : {
        *(.rela.dyn .rela.*)
    } :data

    .dynsym : {
        *(.dynsym)
    } :data

    .dynstr : {
        *(.dynstr)
    } :data

    .hash : {
        *(.hash)
    } :data

    .gnu.hash : {
        *(.gnu.hash)
    } :data

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
        *(.interp)
    }
}
