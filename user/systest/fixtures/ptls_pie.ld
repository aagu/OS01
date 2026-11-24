/*
 * ptls_pie.ld — static PIE fixture with .tdata + .tbss.
 *
 * Mirrors the repository's static-PIE constraints: no interpreter,
 * 4 KiB max page, two disjoint PT_LOAD page ranges, the base-zero RX
 * load carrying the ELF header and PHDR table, and a PT_DYNAMIC that
 * the ELF header/PHDR range plus the RW/NX file-backed load cover.
 * The PT_TLS segment carries .tdata (file-backed, also in the RW
 * load) and .tbss (NOLOAD).
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD FILEHDR PHDRS FLAGS(5);   /* PF_R | PF_X */
    data    PT_LOAD FLAGS(6);                 /* PF_R | PF_W */
    dynamic PT_DYNAMIC FLAGS(6);
    tls     PT_TLS FLAGS(6);
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

    .dynsym : { *(.dynsym) } :data
    .dynstr : { *(.dynstr) } :data
    .hash   : { *(.hash) }   :data
    .gnu.hash : { *(.gnu.hash) } :data

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    . = ALIGN(8);

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
    } :data :tls

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note.*)
        *(.interp)
        *(.group)
    }
}
