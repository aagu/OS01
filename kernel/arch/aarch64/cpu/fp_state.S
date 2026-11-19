.arch armv8-a+fp+simd
.text
.align 2

/* void aarch64_fp_save(void *raw_area)
 * Aligns x0 up to 16 bytes and saves q0..q31, fpsr, fpcr. */
.global aarch64_fp_save
.type aarch64_fp_save, %function
aarch64_fp_save:
    add x0, x0, #15
    and x0, x0, #~15
    stp q0,  q1,  [x0, #(0  * 32)]
    stp q2,  q3,  [x0, #(1  * 32)]
    stp q4,  q5,  [x0, #(2  * 32)]
    stp q6,  q7,  [x0, #(3  * 32)]
    stp q8,  q9,  [x0, #(4  * 32)]
    stp q10, q11, [x0, #(5  * 32)]
    stp q12, q13, [x0, #(6  * 32)]
    stp q14, q15, [x0, #(7  * 32)]
    stp q16, q17, [x0, #(8  * 32)]
    stp q18, q19, [x0, #(9  * 32)]
    stp q20, q21, [x0, #(10 * 32)]
    stp q22, q23, [x0, #(11 * 32)]
    stp q24, q25, [x0, #(12 * 32)]
    stp q26, q27, [x0, #(13 * 32)]
    stp q28, q29, [x0, #(14 * 32)]
    stp q30, q31, [x0, #(15 * 32)]
    mrs x1, fpsr
    mrs x2, fpcr
    add x0, x0, #512
    stp w1, w2, [x0]
    ret
.size aarch64_fp_save, .-aarch64_fp_save

/* void aarch64_fp_restore(const void *raw_area)
 * Aligns x0 up to 16 bytes and restores q0..q31, fpsr, fpcr. */
.global aarch64_fp_restore
.type aarch64_fp_restore, %function
aarch64_fp_restore:
    add x0, x0, #15
    and x0, x0, #~15
    ldp q0,  q1,  [x0, #(0  * 32)]
    ldp q2,  q3,  [x0, #(1  * 32)]
    ldp q4,  q5,  [x0, #(2  * 32)]
    ldp q6,  q7,  [x0, #(3  * 32)]
    ldp q8,  q9,  [x0, #(4  * 32)]
    ldp q10, q11, [x0, #(5  * 32)]
    ldp q12, q13, [x0, #(6  * 32)]
    ldp q14, q15, [x0, #(7  * 32)]
    ldp q16, q17, [x0, #(8  * 32)]
    ldp q18, q19, [x0, #(9  * 32)]
    ldp q20, q21, [x0, #(10 * 32)]
    ldp q22, q23, [x0, #(11 * 32)]
    ldp q24, q25, [x0, #(12 * 32)]
    ldp q26, q27, [x0, #(13 * 32)]
    ldp q28, q29, [x0, #(14 * 32)]
    ldp q30, q31, [x0, #(15 * 32)]
    add x0, x0, #512
    ldp w1, w2, [x0]
    msr fpsr, x1
    msr fpcr, x2
    ret
.size aarch64_fp_restore, .-aarch64_fp_restore

/* void aarch64_fp_zero(void)
 * Resets live hardware q0..q31, fpsr, fpcr to zero (for exec). */
.global aarch64_fp_zero
.type aarch64_fp_zero, %function
aarch64_fp_zero:
    movi v0.16b,  #0
    movi v1.16b,  #0
    movi v2.16b,  #0
    movi v3.16b,  #0
    movi v4.16b,  #0
    movi v5.16b,  #0
    movi v6.16b,  #0
    movi v7.16b,  #0
    movi v8.16b,  #0
    movi v9.16b,  #0
    movi v10.16b, #0
    movi v11.16b, #0
    movi v12.16b, #0
    movi v13.16b, #0
    movi v14.16b, #0
    movi v15.16b, #0
    movi v16.16b, #0
    movi v17.16b, #0
    movi v18.16b, #0
    movi v19.16b, #0
    movi v20.16b, #0
    movi v21.16b, #0
    movi v22.16b, #0
    movi v23.16b, #0
    movi v24.16b, #0
    movi v25.16b, #0
    movi v26.16b, #0
    movi v27.16b, #0
    movi v28.16b, #0
    movi v29.16b, #0
    movi v30.16b, #0
    movi v31.16b, #0
    msr fpsr, xzr
    msr fpcr, xzr
    ret
.size aarch64_fp_zero, .-aarch64_fp_zero
