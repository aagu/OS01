#ifndef LIBC_FLOAT_H
#define LIBC_FLOAT_H

/*
 * libc/include/float.h — completed from compiler builtins (T11).
 *
 * Every constant is the toolchain's own builtin macro, so the header
 * can never drift from the compiler's actual float formats; the
 * static asserts pin the formats the libc assumes (binary32 /
 * binary64 / the x87 80-bit long double on x86_64).  The long double
 * ABI is NOT changed by this port — floatscan targets it as-is.
 *
 * The rounding-macro block is deliberately minimal: OS01 runs single
 * thread, nearest-even, no FENV access — the C default state.
 */

#define FLT_RADIX    2

#define FLT_MANT_DIG  __FLT_MANT_DIG__
#define FLT_MAX_EXP   __FLT_MAX_EXP__
#define FLT_MIN_EXP   __FLT_MIN_EXP__
#define FLT_MAX_10_EXP __FLT_MAX_10_EXP__
#define FLT_MIN_10_EXP __FLT_MIN_10_EXP__
#define FLT_DIG       __FLT_DIG__
#define FLT_EPSILON   __FLT_EPSILON__
#define FLT_MAX       __FLT_MAX__
#define FLT_MIN       __FLT_MIN__
#define FLT_ROUNDS    1            /* round-to-nearest-even */

#define DBL_MANT_DIG  __DBL_MANT_DIG__
#define DBL_MAX_EXP   __DBL_MAX_EXP__
#define DBL_MIN_EXP   __DBL_MIN_EXP__
#define DBL_MAX_10_EXP __DBL_MAX_10_EXP__
#define DBL_MIN_10_EXP __DBL_MIN_10_EXP__
#define DBL_DIG       __DBL_DIG__
#define DBL_EPSILON   __DBL_EPSILON__
#define DBL_MAX       __DBL_MAX__
#define DBL_MIN       __DBL_MIN__
#define DBL_ROUNDS    1

#define LDBL_MANT_DIG  __LDBL_MANT_DIG__
#define LDBL_MAX_EXP   __LDBL_MAX_EXP__
#define LDBL_MIN_EXP   __LDBL_MIN_EXP__
#define LDBL_MAX_10_EXP __LDBL_MAX_10_EXP__
#define LDBL_MIN_10_EXP __LDBL_MIN_10_EXP__
#define LDBL_DIG       __LDBL_DIG__
#define LDBL_EPSILON   __LDBL_EPSILON__
#define LDBL_MAX       __LDBL_MAX__
#define LDBL_MIN       __LDBL_MIN__

#define DECIMAL_DIG   __DECIMAL_DIG__
#define FLT_EVAL_METHOD __FLT_EVAL_METHOD__

#define INFINITY      (__builtin_inff())
#define NAN           (__builtin_nanf(""))

_Static_assert(sizeof(float) == 4, "binary32 required");
_Static_assert(sizeof(double) == 8, "binary64 required");
#if defined(__x86_64__)
/* the ABI floatscan targets; not claimed on other arches */
_Static_assert(sizeof(long double) == 16 && LDBL_MANT_DIG == 64,
               "x87 80-bit long double required by floatscan");
#endif

#endif /* LIBC_FLOAT_H */
