/* libc/arch/aarch64/setjmp.S — AAPCS64 setjmp / longjmp override.
 *
 * Layout (matches libc/include/arch/aarch64/setjmp.h, 22 x 8B = 176B):
 *     0(%x0)  x19, x20
 *    16(%x0)  x21, x22
 *    32(%x0)  x23, x24
 *    48(%x0)  x25, x26
 *    64(%x0)  x27, x28
 *    80(%x0)  x29 (FP), x30 (LR)
 *    96(%x0)  sp
 *   104(%x0)  d8,  d9
 *   120(%x0)  d10, d11
 *   136(%x0)  d12, d13
 *   152(%x0)  d14, d15
 */

    .text
    .global setjmp
    .type setjmp, @function
setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]
    stp     d8,  d9,  [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     w0, #0
    ret
    .size setjmp, . - setjmp

    .global longjmp
    .type longjmp, @function
longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    ldp     d8,  d9,  [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    cmp     w1, #0
    csinc   w0, w1, wzr, ne     /* w0 = (w1 != 0) ? w1 : 1 */
    ret
    .size longjmp, . - longjmp
