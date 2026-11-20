#ifndef _LIBC_ARCH_AARCH64_SETJMP_H
#define _LIBC_ARCH_AARCH64_SETJMP_H

/* AAPCS64 callee-saved state (21 x 64-bit slots, rounded to 22 for 16B alignment):
 *   [0..9]   x19..x28
 *   [10..11] x29 (FP), x30 (LR)
 *   [12]     sp
 *   [13..20] d8..d15 (callee-saved FP/SIMD registers; EL0 has CPACR_EL1.FPEN=0b11) */
typedef unsigned long jmp_buf[22];

#endif /* _LIBC_ARCH_AARCH64_SETJMP_H */
