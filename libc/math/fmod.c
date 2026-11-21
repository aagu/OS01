/* libc/math/fmod.c — exact remainder by integer long division.
 *
 * musl 1.2.5 src/math/fmod.c, with three adaptations: the NaN test is a
 * bit test (not y != y, which compiles to a compare instruction), the
 * NaN path returns the fixed quiet NaN instead of (x*y)/(x*y), and the
 * signed-zero path builds the zero from the bit pattern instead of 0*x.
 * The error paths follow the errno contract: a NaN operand is not an
 * error, an infinite dividend and a zero divisor are domain errors.
 */
#include <errno.h>
#include "musl_libm.h"

double fmod(double x, double y)
{
    union {double f; uint64_t i;} ux = {x}, uy = {y};
    int ex = ux.i>>52 & 0x7ff;
    int ey = uy.i>>52 & 0x7ff;
    int sx = ux.i>>63;
    uint64_t i;

    uint64_t uxi = ux.i;

    /* NaN operand: not an error, the fixed quiet NaN goes out. */
    if ((ex == 0x7ff && (uxi & 0x000fffffffffffffULL) != 0) ||
        (ey == 0x7ff && (uy.i & 0x000fffffffffffffULL) != 0))
        return asdouble(OS01_QNAN_D);

    /* Domain errors: a zero divisor or an infinite dividend. */
    if ((uy.i<<1 == 0) || ex == 0x7ff) {
        errno = EDOM;
        return asdouble(OS01_QNAN_D);
    }

    if (uxi<<1 <= uy.i<<1) {
        if (uxi<<1 == uy.i<<1) {
            ux.i &= 1ULL << 63;      /* signed zero, no FP multiply */
            return ux.f;
        }
        return x;
    }

    /* normalize x and y */
    if (!ex) {
        for (i = uxi<<12; i>>63 == 0; ex--, i <<= 1);
        uxi <<= -ex + 1;
    } else {
        uxi &= -1ULL >> 12;
        uxi |= 1ULL << 52;
    }
    if (!ey) {
        for (i = uy.i<<12; i>>63 == 0; ey--, i <<= 1);
        uy.i <<= -ey + 1;
    } else {
        uy.i &= -1ULL >> 12;
        uy.i |= 1ULL << 52;
    }

    /* x mod y */
    for (; ex > ey; ex--) {
        i = uxi - uy.i;
        if (i >> 63 == 0) {
            if (i == 0) {
                ux.i &= 1ULL << 63;
                return ux.f;
            }
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uy.i;
    if (i >> 63 == 0) {
        if (i == 0) {
            ux.i &= 1ULL << 63;
            return ux.f;
        }
        uxi = i;
    }
    for (; uxi>>52 == 0; uxi <<= 1, ex--);

    /* scale result */
    if (ex > 0) {
        uxi -= 1ULL << 52;
        uxi |= (uint64_t)ex << 52;
    } else {
        uxi >>= -ex + 1;
    }
    uxi |= (uint64_t)sx << 63;
    ux.i = uxi;
    return ux.f;
}