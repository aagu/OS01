/*
 * libc/arch/aarch64/user-pie.ld — AArch64 static-PIE user program layout.
 *
 * Mirrors libc/arch/x86_64/user-pie.ld: links at offset 0 (ET_DYN) with
 * PT_DYNAMIC + .rela.dyn (R_AARCH64_RELATIVE only), no PT_INTERP.
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
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
        *(.rodata .rodata.*)
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
