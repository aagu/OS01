/* libc/math/sqrt.c — correctly rounded square root by fixed-point
 * Goldschmidt iteration (musl 1.2.5 src/math/sqrt.c).
 *
 * Adaptations: FENV_SUPPORT is 0 (OS01 has no fenv, so the final
 * rounding-mode nudge is gone), the invalid path returns the fixed
 * quiet NaN and sets EDOM for a negative argument, and the subnormal
 * normalization rebuilds the bit pattern with integer operations
 * instead of the x * 0x1p52 multiply.
 */
#include <errno.h>
#include "musl_libm.h"
#include "sqrt_data.h"

static inline uint32_t mul32(uint32_t a, uint32_t b)
{
    return (uint64_t)a*b >> 32;
}

static inline uint64_t mul64(uint64_t a, uint64_t b)
{
    uint64_t ahi = a>>32;
    uint64_t alo = a&0xffffffff;
    uint64_t bhi = b>>32;
    uint64_t blo = b&0xffffffff;
    return ahi*bhi + (ahi*blo >> 32) + (alo*bhi >> 32);
}

double sqrt(double x)
{
    uint64_t ix, top, m;

    ix = asuint64(x);
    top = ix >> 52;
    if (predict_false(top - 0x001 >= 0x7ff - 0x001)) {
        /* x < 0x1p-1022 or inf or nan.  */
        if (ix * 2 == 0)
            return x;
        if (ix == 0x7ff0000000000000)
            return x;                                  /* +inf */
        if ((ix & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL)
            return asdouble(OS01_QNAN_D);               /* NaN input, any sign */
        if (ix >> 63) {                                /* negative: domain error */
            errno = EDOM;
            return asdouble(OS01_QNAN_D);
        }
        /* x is subnormal: normalize it with integer operations.
         * The value is m * 2^-1074; the normalized form is
         * (1 + frac) * 2^(p-1074) with p the highest set bit of m. */
        {
            uint64_t mm = ix & 0x000fffffffffffffULL;
            int p = 63 - __builtin_clzll(mm);
            uint64_t frac = (mm - (1ULL << p)) << (52 - p);
            uint64_t biased = (uint64_t)(p + 1);
            ix = (biased << 52) | frac;
            top = ix >> 52;
            top -= 52;
        }    }

    /* argument reduction: x = 4^e m; with integer e, and m in [1, 4) */
    int even = top & 1;
    m = (ix << 11) | 0x8000000000000000;
    if (even) m >>= 1;
    top = (top + 0x3ff) >> 1;

    static const uint64_t three = 0xc0000000;
    uint64_t r, s, d, u, i;

    i = (ix >> 46) % 128;
    r = (uint32_t)__os01_libm_rsqrt_tab[i] << 16;
    s = mul32(m>>32, r);
    d = mul32(s, r);
    u = three - d;
    r = mul32(r, u) << 1;
    s = mul32(s, u) << 1;
    d = mul32(s, r);
    u = three - d;
    r = mul32(r, u) << 1;
    r = r << 32;
    s = mul64(m, r);
    d = mul64(s, r);
    u = (three<<32) - d;
    s = mul64(s, u);  /* repr: 3.61 */
    s = (s - 2) >> 9; /* repr: 12.52 */

    /* compute the nearest rounded result */
    uint64_t d0, d1;
    double y;
    d0 = (m << 42) - s*s;
    d1 = s - d0;
    s += d1 >> 63;
    s &= 0x000fffffffffffff;
    s |= top << 52;
    y = asdouble(s);
    return y;
}