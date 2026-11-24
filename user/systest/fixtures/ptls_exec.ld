/*
 * ptls_exec.ld — fixed-address ET_EXEC fixture with a pure-.tbss
 * PT_TLS segment (TinyGo 0.44.0 shape).
 *
 * The data PT_LOAD carries .data/.bss; .tdata (empty here) and .tbss
 * belong to the PT_TLS segment only, so the loader sees
 * p_filesz == 0 and must not read a template.  The link-time thread
 * pointer is align_up(.tbss end, p_align) inside the user window.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
    tls  PT_TLS  FLAGS(6);
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text.start)
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    . = ALIGN(8);

    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
    } :tls

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note.*)
        *(.group)
        *(.rela.*)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
    }
}
