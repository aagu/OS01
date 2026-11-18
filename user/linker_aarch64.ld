/*
 * user/linker_aarch64.ld — AArch64 user program layout.
 *
 * Structural rules mirror user/linker.ld (x86) and must satisfy the
 * kernel's aarch64 image validator (kernel/arch/aarch64/memory/user_elf.c):
 *   - PT_LOAD segment vaddrs are 4 KiB aligned,
 *   - every PT_LOAD carries PF_R, and no segment is W+X,
 *   - the entry point sits inside a PF_X segment's file range,
 *   - no PT_INTERP / PT_DYNAMIC / PT_TLS.
 * The base 0x400000 sits below the user stack window (0x20400000) and the
 * mmap window (USER_VM_MMAP_BASE = 0x40000000); the kernel installs the
 * heap VMA above the loaded segments.
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W */
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
        *(.dynamic)
        *(.dynsym)
    }
}
