/* libc/math/round.c — integer bit-manipulation round (half away from zero).
 *
 * C99 7.12.9.7: round rounds halfway cases away from zero, which is what
 * the fraction comparison below implements without any FP arithmetic.
 */
#include "musl_libm.h"

double round(double x)
{
    union {double f; uint64_t i;} u = {x};
    uint64_t sign = u.i & (1ULL << 63);
    uint64_t mag  = u.i & ~(1ULL << 63);
    int e = (int)(mag >> 52 & 0x7ff) - 0x3ff;

    if (mag == 0)                       /* +-0 */
        return x;
    if (e >= 52)                        /* |x| >= 2^52: already integral */
        return x;
    if (e < 0) {                        /* |x| < 1 */
        if (mag >= 0x3fe0000000000000ULL)   /* |x| >= 0.5 */
            return sign ? -1.0 : 1.0;
        return sign ? -0.0 : 0.0;
    }
    {
        uint64_t step = 1ULL << (52 - e);      /* one ulp at this exponent */
        uint64_t frac = mag & (step - 1);
        mag &= ~(step - 1);                    /* drop the fraction FIRST */
        if (frac >= (step >> 1))               /* >= half: away from zero */
            mag += step;                       /* may carry into the exponent */
        return asdouble(sign | mag);
    }
}