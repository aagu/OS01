/* aarch64 kernel linker script.
 *
 * THIS FILE IS A PREPROCESSED TEMPLATE: the AArch64 build generates
 * $(KERNEL_BUILD_DIR)/linker.generated.ld from it via the C preprocessor
 * with the same -D layout flags as kernel C and assembly
 * (AARCH64_LAYOUT_CFLAGS). Feed it to ld only through that generated
 * script; the raw file is not directly linkable.
 *
 * Layout per the aarch64 phase-1 spec (§2.5, §2.2, §4) and the
 * 2026-11-17 LMA platform-config spec (§2):
 *
 *   .boot   : VMA = LMA = AARCH64_KERNEL_ENTRY (low physical boot block B,
 *             default RAM base 0x40000000 + 512 KiB)
 *             Holds head.S + secondary trampoline + early-stack/page-table
 *             storage + DTB slot + aarch64_boot_percpu[].  The UEFI loader
 *             allocates by p_paddr and jumps to ENTRY(_start).
 *
 *   .text / .rodata / .data / .bss : VMA = high half (AARCH64_PAGE_OFFSET +
 *             AARCH64_KERNEL_ENTRY + _boot_size), LMA = sequential after
 *             .boot.  The loader only needs the LMA bytes in low RAM; the
 *             high-half page table we build remaps them.
 *
 *   The dual LMA layout (low bytes, high VMA) is what makes the
 *   "identity copy at MMU-on" trick unnecessary.
 */

#include <arch/aarch64/boot_layout.h>

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
 * AARCH64_PAGE_OFFSET is 0xffff000000000000; for the default load
 * address 0x40080000 that yields 0xffff0000_40080000.  Sections after
 * .boot live here virtually and at sequential low LMAs physically. */
_kernel_high_vma = AARCH64_PAGE_OFFSET + AARCH64_KERNEL_ENTRY;
_kernel_page_offset = AARCH64_PAGE_OFFSET;

SECTIONS
{
    /* ── Low-physical .boot segment (VMA = LMA) ───────────────────── */
    . = AARCH64_KERNEL_ENTRY;

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

    .rodata ALIGN(0x1000) : AT(ADDR(.rodata) - _kernel_page_offset) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    } :text

    /* Consume this exact input before the .data.* wildcard; no other data
     * subsection is filtered out.  _data_start/_data_end bracket the full
     * writable-data span, including this aligned task union and initcalls. */
    . = ALIGN(32768);
    _data_start = .;
    .data.init_task : AT(ADDR(.data.init_task) - _kernel_page_offset) {
        KEEP(*(.data.init_task))
    } :data
    ASSERT((ADDR(.data.init_task) & 32767) == 0,
           "aarch64 init_task_union must be STACK_SIZE aligned")
    ASSERT(SIZEOF(.data.init_task) >= 32768,
           "aarch64 init_task_union must reserve at least STACK_SIZE")

    . = ALIGN(0x1000);
    .data ALIGN(0x1000) : AT(ADDR(.data) - _kernel_page_offset) {
        *(.data)
        *(.data.*)
    } :data

    /* KEEP-wrapped: drivers register a function pointer here via
     * SUBSYS_INITCALL(); arch_register_subsys() iterates this range
     * to call each one. Placement AFTER .data (not before) to byte-mirror
     * kernel/arch/x86_64/linker.ld — same relative position keeps
     * the kernel-image walk in pmm.c consistent across architectures.
     * The AT(ADDR(...) - _kernel_page_offset) mirrors the surrounding
     * .data section's high-half direct-map LMA mapping so the symbols
     * are addressable from the kernel image's high-half VA. */
    .subsys_init : AT(ADDR(.subsys_init) - _kernel_page_offset)
    {
        PROVIDE(__subsys_init_start = .);
        KEEP(*(.subsys_init))
        PROVIDE(__subsys_init_end = .);
    } :data
    _data_end = .;

    . = ALIGN(0x1000);
    /* The loader reserves/zeros physical LOAD addresses. Keep page-aligned
     * high-half BSS at the physical address our direct mapping accesses. */
    .bss ALIGN(0x1000) (NOLOAD) : AT(ADDR(.bss) - _kernel_page_offset) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.high.bss)
        *(COMMON)
        _bss_end = .;
    } :data

    _kernel_end = .;
    _kernel_lma_end = LOADADDR(.bss) + SIZEOF(.bss);
    ASSERT(_kernel_lma_end <= AARCH64_HANDOFF_BASE, "kernel overlaps UEFI handoff")
    ASSERT(_start == AARCH64_KERNEL_ENTRY, "aarch64 _start does not match the configured boot block entry")

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
