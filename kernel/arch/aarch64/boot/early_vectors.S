.section .boot.text, "ax"
.globl boot_vectors
.balign 2048
boot_vectors:
    .balign 128
    b   .Lpanic                       /* sync, SP_EL0 */
    .balign 128
    b   .Lpanic                       /* IRQ,   SP_EL0 */
    .balign 128
    b   .Lpanic                       /* FIQ,   SP_EL0 */
    .balign 128
    b   .Lpanic                       /* SError, SP_EL0 */
    .balign 128
    b   .Lpanic                       /* sync,  SP_ELx */
    .balign 128
    b   .Lirq_spin                   /* IRQ,   SP_ELx (left for parity) */
    .balign 128
    b   .Lpanic                       /* FIQ,   SP_ELx */
    .balign 128
    b   .Lpanic                       /* SError, SP_ELx */

.Lpanic:
    mov     x1, #'P'
    bl      .Lprint_c
    mov     x1, #'!'
    bl      .Lprint_c
.Lpanic_spin:
    b       .Lpanic_spin

.Lirq_spin:
    b       .Lirq_spin

/* Helpers — all clobber x0,x1,x2,x3. Console PA comes from the parked
 * early-MMIO handoff (loader-derived from the firmware FDT), falling
 * back to the QEMU virt PL011 when no handoff was parked. Both pre-MMU
 * (PA access) and post-MMU (identity Device window) use the same
 * address; a zero slot reads as "no handoff" even before the .boot.bss
 * clear because the loader zeroes the image BSS on load. */
.Lprint_c:
    ldr     x2, =aarch64_boot_early_mmio_console
    ldr     x2, [x2]
    cbnz    x2, 9f
    mov     x2, #0x09000000
9:  /* spin until TXFF=0, then write char in x1 */
1:  ldr     w3, [x2, #0x18]
    tst     w3, #(1 << 5)
    b.ne    1b
    str     w1, [x2]
    ret

/* Global entry for callers outside this TU (the head.S fail-closed
 * checks print the W! marker through it). */
.globl aarch64_boot_print_c
.hidden aarch64_boot_print_c
aarch64_boot_print_c:
    b       .Lprint_c

.Lprint_x64:
    /* print 16 hex digits of x0, MSB first.  Uses x9 as counter,
     * but BL clobbers caller-saved regs, so save x9 (and x30 — the
     * inner .Lprint_c call clobbers the link register) on the stack.
     * "ror x0, x0, #60" is a rotate-left by 4: it moves the next
     * (more significant) nibble down into the low four bits. */
    stp     x9, x30, [sp, #-16]!
    mov     x9, #16
1:  ror     x0, x0, #60
    and     x1, x0, #0xf
    cmp     x1, #10
    b.lt    2f
    add     x1, x1, #('a' - 10)
    b       3f
2:  add     x1, x1, #'0'
3:  bl      .Lprint_c
    sub     x9, x9, #1
    cbnz    x9, 1b
    ldp     x9, x30, [sp], #16
    ret

.globl aarch64_boot_check_midr
.hidden aarch64_boot_check_midr
aarch64_boot_check_midr:
    mrs     x0, midr_el1
    lsr     x1, x0, #16
    and     x1, x1, #0xf
    cmp     x1, #0xf
    b.ne    .Lmidr_fatal
    ret

/* Fatal: the boot CPU is not an AArch64 core (MIDR_EL1 architecture
 * != 0xF).  The kernel logger is not installed this early, so print
 * straight to PL011 — otherwise this fail-closed path is completely
 * silent and the only symptom is a missing boot marker. */
.Lmidr_fatal:
    /* x5 = string cursor (x1 is the character argument to .Lprint_c);
     * x4 = the byte read. */
    adr     x5, aarch64_midr_fatal_msg
.Lmidr_fatal_loop:
    ldrb    w4, [x5], #1
    cbz     w4, .Lmidr_fatal_hex
    mov     x1, x4
    bl      .Lprint_c
    b       .Lmidr_fatal_loop
.Lmidr_fatal_hex:
    mrs     x0, midr_el1
    bl      .Lprint_x64
    mov     x1, #'\n'
    bl      .Lprint_c
.Lmidr_fatal_spin:
    b       .Lmidr_fatal_spin

.section .boot.rodata, "a"
.balign 4
.globl aarch64_midr_fatal_msg
aarch64_midr_fatal_msg:
    .asciz  "FATAL: not AArch64, MIDR="
