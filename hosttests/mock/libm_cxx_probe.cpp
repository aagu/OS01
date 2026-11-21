/* hosttests/mock/libm_cxx_probe.cpp — the freestanding C++ half of the
 * public math.h contract.
 *
 * Two things are proved here, and the first one is proved by LINKING:
 *
 * 1. C LINKAGE: the probe includes the PUBLIC math.h and calls fabs.
 *    It is compiled with the same -Dfabs=os01_fabs rename the
 *    production objects use, so the reference has to match the renamed
 *    archive symbol:
 *      - with extern "C" in the public header the reference is the
 *        plain `os01_fabs` and the link succeeds;
 *      - without it the reference is the C++ mangled `_Z9os01_fabsd`
 *        and the link fails with an undefined reference.
 *    The link itself is the proof; the test only checks the value the
 *    probe returns.
 *
 * 2. The classification macros in C++: C++ dispatches them by overload
 *    instead of _Generic, so the integer fallback, the three floating
 *    point types and the single-evaluation rule are exercised here.
 */

#include <math.h>

#include <float.h>

extern "C" double os01_cxx_probe_fabs(double x)
{
    return fabs(x);
}

static int side_effects;

static double bump(double v)
{
    side_effects++;
    return v;
}

extern "C" int os01_cxx_probe_classify(void)
{
    int fail = 0;

    /* the integer fallback: an int is classified through double */
    if (!isnormal(5)) fail |= 1;
    if (isnormal(0)) fail |= 2;
    if (!isfinite(5)) fail |= 4;
    if (signbit(-1) != 1) fail |= 8;

    /* the three floating point types keep their own type */
    float tiny = 1e-45f;
    if (fpclassify(tiny) != FP_SUBNORMAL) fail |= 16;
    if (fpclassify(1.0f) != FP_NORMAL) fail |= 32;
    if (fpclassify(0.0) != FP_ZERO) fail |= 64;
    if (!signbit(-0.0)) fail |= 128;

    /* the long double path reads the object representation */
    long double lmax = LDBL_MAX;
    if (!isfinite(lmax)) fail |= 256;
    if (!isnormal(lmax)) fail |= 512;
    if (!isinf(__builtin_infl())) fail |= 1024;
    if (!isnan(__builtin_nanl(""))) fail |= 2048;

    /* the argument is evaluated exactly once */
    side_effects = 0;
    (void)isfinite(bump(1.0));
    if (side_effects != 1) fail |= 4096;

    return fail;
}
