/* libc/math/ceil.c — integer bit-manipulation ceil.  See floor.c. */
#include "musl_libm.h"

double ceil(double x)
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
        if (u.i >> 63 == 0)
            u.i += m;
        u.i &= ~m;
    } else {
        if (u.i >> 63)
            u.f = -0.0;
        else if (u.i << 1)
            u.f = 1.0;
    }
    return u.f;
}