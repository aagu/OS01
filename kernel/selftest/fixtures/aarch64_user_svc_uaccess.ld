/* kernel/selftest/fixtures/aarch64_user_svc_uaccess.ld
 *
 * One text page and one RW PT_LOAD spanning exactly two pages.  The
 * page right after the RW segment (0x403000) stays unmapped, which is
 * what the payload's cross-page -EFAULT check relies on.
 */
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);          /* R + X */
    data PT_LOAD FLAGS(6);          /* R + W */
}
SECTIONS {
    .text 0x400000 : { *(.text) } :text
    /* Page 1 of the writable segment: file-backed and padded to the
     * page end so the boundary buffer at 0x401ff8 lives here. */
    .data 0x401000 : { *(.data) . = 0x402000 ; } :data
    /* Page 2: BSS, padded to 0x403000.  The page after it is the
     * unmapped one the payload writes across. */
    .bss 0x402000 : { *(.bss) . = 0x403000 ; } :data
}
