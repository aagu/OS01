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

#endif /* LIBC_MATH_H */
