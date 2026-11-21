/* libc/math/fmodf.c — exact remainder by integer long division.
 *
 * musl 1.2.5 src/math/fmodf.c, with the same three adaptations as fmod.c:
 * the NaN test is a bit test (not y != y, which compiles to a compare
 * instruction), the NaN path returns the fixed quiet NaN instead of
 * (x*y)/(x*y), and the signed-zero path builds the zero from the bit
 * pattern instead of 0*x.  The error paths follow the errno contract:
 * a NaN operand is not an error, an infinite dividend and a zero
 * divisor are domain errors.
 */
#include <errno.h>
#include "musl_libm.h"

float fmodf(float x, float y)
{
    union {float f; uint32_t i;} ux = {x}, uy = {y};
    int ex = ux.i>>23 & 0xff;
    int ey = uy.i>>23 & 0xff;
    uint32_t sx = ux.i & 0x80000000;
    uint32_t i;
    uint32_t uxi = ux.i;

    /* NaN operand: not an error, the fixed quiet NaN goes out. */
    if ((ex == 0xff && (uxi & 0x007fffffU) != 0) ||
        (ey == 0xff && (uy.i & 0x007fffffU) != 0))
        return asfloat(OS01_QNAN_F);

    /* Domain errors: a zero divisor or an infinite dividend. */
    if ((uy.i<<1 == 0) || ex == 0xff) {
        errno = EDOM;
        return asfloat(OS01_QNAN_F);
    }

    if (uxi<<1 <= uy.i<<1) {
        if (uxi<<1 == uy.i<<1) {
            ux.i &= 1U << 31;         /* signed zero, no FP multiply */
            return ux.f;
        }
        return x;
    }

    /* normalize x and y */
    if (!ex) {
        for (i = uxi<<9; i>>31 == 0; ex--, i <<= 1);
        uxi <<= -ex + 1;
    } else {
        uxi &= -1U >> 9;
        uxi |= 1U << 23;
    }
    if (!ey) {
        for (i = uy.i<<9; i>>31 == 0; ey--, i <<= 1);
        uy.i <<= -ey + 1;
    } else {
        uy.i &= -1U >> 9;
        uy.i |= 1U << 23;
    }

    /* x mod y */
    for (; ex > ey; ex--) {
        i = uxi - uy.i;
        if (i >> 31 == 0) {
            if (i == 0) {
                ux.i &= 1U << 31;
                return ux.f;
            }
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uy.i;
    if (i >> 31 == 0) {
        if (i == 0) {
            ux.i &= 1U << 31;
            return ux.f;
        }
        uxi = i;
    }
    for (; uxi>>23 == 0; uxi <<= 1, ex--);

    /* scale result up */
    if (ex > 0) {
        uxi -= 1U << 23;
        uxi |= (uint32_t)ex << 23;
    } else {
        uxi >>= -ex + 1;
    }
    uxi |= sx;
    ux.i = uxi;
    return ux.f;
}