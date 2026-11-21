/* libc/math/roundf.c — integer bit-manipulation roundf.  See round.c. */
#include "musl_libm.h"

float roundf(float x)
{
    union {float f; uint32_t i;} u = {x};
    uint32_t sign = u.i & (1U << 31);
    uint32_t mag  = u.i & ~(1U << 31);
    int e = (int)(mag >> 23 & 0xff) - 0x7f;

    if (mag == 0)
        return x;
    if (e >= 23)
        return x;
    if (e < 0) {
        if (mag >= 0x3f000000U)            /* |x| >= 0.5 */
            return sign ? -1.0f : 1.0f;
        return sign ? -0.0f : 0.0f;
    }
    {
        uint32_t step = 1U << (23 - e);
        uint32_t frac = mag & (step - 1);
        mag &= ~(step - 1);                /* drop the fraction FIRST */
        if (frac >= (step >> 1))
            mag += step;
        return asfloat(sign | mag);
    }
}