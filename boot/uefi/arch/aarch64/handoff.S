    .text
    .p2align 2

    /* aarch64_enter_kernel(entry=x0, context_phys=x1, trampoline_phys=x2,
     *                       beacon_pa=x3, fb_base=x4, fb_stride=x5,
     *                       fb_pixel=x6)
     * — the C wrapper derives the trampoline page PA from the shared
     * layout header; no fixed address may leak into this file. x3 is the
     * firmware-initialized UART's PA for the bring-up beacon, or 0 to
     * skip it (the default build and anything without that knob). x4-x6
     * are an optional framebuffer checkpoint: after the UART store, a
     * 16x16 block at (40,140) proves the CPU survived the store. */
    .global aarch64_enter_kernel
aarch64_enter_kernel:
    br      x2

    .p2align 2
    .global aarch64_handoff_stub_start
    .global aarch64_handoff_stub_end
aarch64_handoff_stub_start:
    msr     daifset, #0xf
    dsb     sy
    mrs     x16, sctlr_el1
    bic     x16, x16, #1
    msr     sctlr_el1, x16
    isb
    tlbi    vmalle1
    dsb     sy
    isb
    /* "Handoff reached": one write-only physical store, MMU off, so no
     * page-table walk is involved. A host with no device at that address
     * drops the store rather than faulting. */
    cbz     x3, 1f
    mov     w16, #'H'
    str     w16, [x3]
    mov     w16, #'\r'
    str     w16, [x3]
    mov     w16, #'\n'
    str     w16, [x3]
1:
    /* Post-store checkpoint: 16x16 pixels at (40,140), physical store */
    cbz     x4, 2f
    mov     x7, #0
3:  add     x9, x7, #140
    mul     x9, x9, x5
    add     x9, x9, #40
    lsl     x9, x9, #2
    add     x9, x4, x9
    mov     x10, #16
    mov     x11, #0
4:  str     w6, [x9, x11, lsl #2]
    add     x11, x11, #1
    cmp     x11, x10
    b.ne    4b
    add     x7, x7, #1
    cmp     x7, #16
    b.ne    3b
2:
    mov     x16, x0
    mov     x0, x1
    br      x16
aarch64_handoff_stub_end:
