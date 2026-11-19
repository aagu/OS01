#ifndef OS01_AARCH64_FP_H
#define OS01_AARCH64_FP_H

/* AArch64 FP/SIMD context save area and assembly helpers.
 *
 * Layout (16-byte aligned, 528 bytes total):
 *   [0 .. 511]   q0..q31 (32 x 128-bit SIMD/FP registers)
 *   [512 .. 515] fpsr    (32-bit Floating-point Status Register)
 *   [516 .. 519] fpcr    (32-bit Floating-point Control Register)
 *   [520 .. 527] reserved padding (0)
 *
 * Because kmalloc() may return an 8-byte aligned block, callers pass the
 * raw pointer returned by fpu_area_alloc() (which allocates
 * AARCH64_FP_STATE_SIZE + 16 bytes) and the helpers align up to 16 bytes.
 */

#define AARCH64_FP_STATE_SIZE 528

void aarch64_fp_save(void *raw_area);
void aarch64_fp_restore(const void *raw_area);
void aarch64_fp_zero(void);

#endif /* OS01_AARCH64_FP_H */
