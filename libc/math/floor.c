/* libc/math/floor.c — integer bit-manipulation floor.
 *
 * Ported from musl 1.2.5 src/math/floorf.c (the float variant is pure
 * bit manipulation there; the double variant uses the 1/EPS toint trick,
 * which depends on the compiler's excess-precision semantics and would
 * drag long double into the userland on x86_64).
 */
#include "musl_libm.h"

double floor(double x)
{
    union {double f; uint64_t i;} u = {x};
    int e = (int)(u.i >> 52 & 0x7ff) - 0x3ff;
    uint64_t m;

    if (e >= 52)
        return x;
    if (e >= 0) {
        m = 0x000fffffffffffffULL >> e;
        if ((u.i & m) == 0)
            return x;
        if (u.i >> 63)
            u.i += m;
        u.i &= ~m;
    } else {
        if (u.i >> 63 == 0)
            u.i = 0;
        else if (u.i << 1)
            u.f = -1.0;
    }
    return u.f;
}