/* aarch64 kernel linker script.
 *
 * Layout per the aarch64 phase-1 spec (§2.5, §2.2, §4):
 *
 *   .boot   : VMA = LMA = 0x40080000 (low physical, RAM base 0x40000000 + 512 KiB)
 *             Holds head.S + secondary trampoline + early-stack/page-table
 *             storage + DTB slot + aarch64_boot_percpu[].  QEMU `-kernel`
 *             loads by LMA and jumps to ENTRY(_start).
 *
 *   .text / .rodata / .data / .bss : VMA = high half (0xffff0000_40080000 +
 *             _boot_size), LMA = sequential after .boot.  QEMU only needs
 *             the LMA bytes in low RAM; the high-half page table we build
 *             remaps them.
 *
 *   The dual LMA layout (low bytes, high VMA) is what makes the
 *   "identity copy at MMU-on" trick unnecessary.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    boot PT_LOAD FLAGS(0x7);       /* PF_R | PF_W | PF_X  */
    text PT_LOAD FLAGS(0x5);       /* PF_R | PF_X          */
    data PT_LOAD FLAGS(0x6);       /* PF_R | PF_W          */
}

/* High-half "kernel mirror" VMA: ARCH_PAGE_OFFSET + low load address.
 *
 * The spec sets ARCH_PAGE_OFFSET to 0xffff000000000000; for our load
 * address 0x40080000 that yields 0xffff0000_40080000.  Sections after
 * .boot live here virtually and at sequential low LMAs physically. */
_kernel_high_vma = 0xffff000040080000;

SECTIONS
{
    /* ── Low-physical .boot segment (VMA = LMA) ───────────────────── */
    . = 0x40080000;

    _boot_start = .;

    .boot.text ALIGN(0x1000) : {
        KEEP(*(.boot.text.first))
        KEEP(*(.boot.text))
        *(.boot.text.*)
        *(.text.boot)
    } :boot

    .boot.rodata ALIGN(0x1000) : {
        *(.boot.rodata)
        *(.boot.rodata.*)
        *(.rodata.boot)
    } :boot

    .boot.data ALIGN(0x1000) : {
        *(.boot.data)
        *(.boot.data.*)
        *(.data.boot)
    } :boot

    .boot.bss ALIGN(0x1000) (NOLOAD) : {
        _boot_bss_start = .;
        KEEP(*(.boot.bss))
        KEEP(*(.boot.bss.*))
        KEEP(*(.bss.boot))
    } :boot
    _boot_bss_end = .;
    /* Align the .boot region end to a page so that .text's LMA and VMA keep
     * the identity relationship VMA = LMA + ARCH_PAGE_OFFSET.  Without this,
     * VMA (ALIGNed up) and LMA (= _boot_end, unaligned) diverge by <4 KiB and
     * the high-half direct-map in head.S (identity) reads the wrong bytes. */
    . = ALIGN(0x1000);
    _boot_end = .;
    _boot_size = _boot_end - _boot_start;
    _high_load_offset = _boot_size;

    . = _kernel_high_vma + _boot_size;

    /* Convenience: anything tagged __attribute__((section(".high.text"))) */
    .text ALIGN(0x1000) : AT(_boot_end) {
        _text_start = .;
        *(.text)
        *(.text.*)
        *(.high.text)
        _text_end = .;
    } :text

    .rodata ALIGN(0x1000) : AT(LOADADDR(.text) + SIZEOF(.text)) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    } :text

    . = ALIGN(0x1000);
    .data ALIGN(0x1000) : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    } :data

    . = ALIGN(0x1000);
    .bss ALIGN(0x1000) (NOLOAD) : AT(LOADADDR(.data) + SIZEOF(.data)) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.high.bss)
        *(COMMON)
        _bss_end = .;
    } :data

    _kernel_end = .;
    _kernel_lma_end = LOADADDR(.bss) + SIZEOF(.bss);

    /* Sentinel: a couple of known symbols used by head.S to size .boot.bss
     * clearing and to compute page-table addresses. */
    _dtb_slot_lma = _boot_end - 0x10;        /* last 16 bytes of .boot region */
    _page_tables_lma = _boot_start;          /* start of .boot area       */

    /* Discard debug-only sections that the freestanding kernel doesn't need. */
    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.eh_frame_hdr)
        *(.ARM.exidx .ARM.exidx.*)
        *(.ARM.attributes .ARM.attributes.*)
        *(.comment)
        *(.llvm_addrsig*)
    }
}
