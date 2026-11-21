/* libc/math/musl_libm.h — private helpers for the vendored musl math subset.
 *
 * This header is NOT public: it lives next to the sources that use it and
 * is never installed into the sysroot (only libc/include/ is published).
 * It replaces musl's src/internal/libm.h, which carries musl-internal
 * machinery (long double unions, fp_arch.h, WANT_ROUNDING) that OS01
 * does not have.
 */
#ifndef OS01_LIBM_MUSL_LIBM_H
#define OS01_LIBM_MUSL_LIBM_H

#include <stdint.h>

/* ── bit casts (musl's asuint64/asdouble/asuint/asfloat) ─────── */
static inline double asdouble(uint64_t u)
{
    union { uint64_t i; double d; } v;
    v.i = u;
    return v.d;
}

static inline uint64_t asuint64(double x)
{
    union { uint64_t i; double d; } v;
    v.d = x;
    return v.i;
}

static inline float asfloat(uint32_t u)
{
    union { uint32_t i; float f; } v;
    v.i = u;
    return v.f;
}

static inline uint32_t asuint(float x)
{
    union { uint32_t i; float f; } v;
    v.f = x;
    return v.i;
}

/* ── branch hints (musl's predict_true/predict_false) ─────────── */
#define predict_true(x)  __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)

/* ── the fixed quiet NaN every OS01 math function returns ───────
 *
 * The pattern is positive on purpose: the x86 hardware default NaN
 * carries a negative sign while the ARM one does not, so a fixed
 * positive pattern is the only choice that is identical on both
 * targets.
 */
#define OS01_QNAN_D 0x7ff8000000000000ULL
#define OS01_QNAN_F 0x7fc00000U

#endif /* OS01_LIBM_MUSL_LIBM_H */