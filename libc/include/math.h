#ifndef LIBC_MATH_H
#define LIBC_MATH_H

#include <stdint.h>

#define INFINITY (__builtin_inf())
#define NAN (__builtin_nan(""))

static inline int math_fp_signbit(double x)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    return (int)((v.u >> 63) & 1u);
}

static inline int math_fp_isfinite(double x)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    uint32_t exp = (uint32_t)((v.u >> 52) & 0x7FFu);
    return exp != 0x7FFu ? 1 : 0;
}

#define signbit(x) math_fp_signbit(x)
#define isfinite(x) math_fp_isfinite(x)

/* fabs: the absolute value of a binary64.
 *
 * The absolute value of a binary64 is the number with the SIGN BIT
 * cleared and every other bit unchanged — that is what IEEE-754 says it
 * is, and it is exactly what the code below does:
 *
 *   -0.0   → +0.0     (the value stays zero, the sign bit is cleared)
 *   -Inf   → +Inf
 *   NaN    → NaN      (the payload survives, the sign bit is cleared)
 *
 * It is a header inline on purpose: the libc has no libm archive, and a
 * header inline can never be replaced by another library's fabs at link
 * time.  The declaration and the definition are the same object, so a
 * consumer that renames fabs (the host tests do) renames both of them
 * consistently. */
static inline double fabs(double x)
{
    union { double d; uint64_t u; } v;

    v.d = x;
    v.u &= ~(1ull << 63);
    return v.d;
}

#endif /* LIBC_MATH_H */
