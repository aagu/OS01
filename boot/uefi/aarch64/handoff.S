    .text
    .p2align 2

    .global aarch64_enter_kernel
aarch64_enter_kernel:
    movz    x16, #0xf000
    movk    x16, #0x401f, lsl #16
    br      x16

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
    mov     x16, x0
    mov     x0, x1
    br      x16
aarch64_handoff_stub_end:
