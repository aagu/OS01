#ifndef _LIBC_ARCH_FLOAT_ABI_H
#define _LIBC_ARCH_FLOAT_ABI_H

/* Arch-neutral floating-point scanner ABI facade.
 *
 * Encapsulates the internal scanner accumulator type (fs_float_t),
 * precision constants, bit-level NaN/scaling/fmod helpers, and
 * narrowing/widening conversions to float / double / long double.
 *
 *   x86_64 : hardware x87 80-bit long double (LDBL_MANT_DIG == 64).
 *   aarch64: hardware IEEE 754 binary64 double (DBL_MANT_DIG == 53) with
 *            pure-integer binary64 -> binary128 bit encoding for long double
 *            so freestanding userland needs no soft-quad compiler-rt calls.
 */
#if defined(__x86_64__)
#include <libc_arch/x86_64/float_abi.h>
#elif defined(__aarch64__)
#include <libc_arch/aarch64/float_abi.h>
#else
#error "Unsupported architecture for libc float_abi facade"
#endif

#endif /* _LIBC_ARCH_FLOAT_ABI_H */
