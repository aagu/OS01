#ifndef _LIBC_ARCH_AARCH64_FLOAT_ABI_H
#define _LIBC_ARCH_AARCH64_FLOAT_ABI_H

#include <float.h>
#include <stdint.h>

/* aarch64-none-elf defines long double as IEEE 754 binary128 (quad), which has
 * no hardware arithmetic on ARMv8-A and would emit __addtf3/__multf3/__extenddftf2
 * calls absent from OS01's selfhosted runtime.  Run the scanner core in hardware
 * binary64 (double) and encode binary128 long double outputs via pure integer
 * bit manipulation (zero compiler-rt dependencies). */
typedef double fs_float_t;

#define FS_MANT_DIG   DBL_MANT_DIG
#define FS_MAX_EXP    DBL_MAX_EXP
#define FS_MIN_EXP    DBL_MIN_EXP
#define FS_MAX        DBL_MAX
#define FS_MIN        DBL_MIN
#define FS_EPSILON    DBL_EPSILON
#define FS_LDBL_MIN   DBL_MIN

/* The absolute value is the number with the sign bit cleared and every
 * other bit unchanged, so it is computed with integer bit operations:
 * the scanner core never asks the FPU to do arithmetic. */
static inline fs_float_t fs_fabs(fs_float_t x)
{
    union { double f; uint64_t i; } ux = {x};
    ux.i &= 0x7fffffffffffffffULL;
    return ux.f;
}

static inline fs_float_t fs_copysignl(fs_float_t x, fs_float_t y)
{
    union { double f; uint64_t i; } ux = {x}, uy = {y};
    ux.i &= 0x7fffffffffffffffULL;
    ux.i |= uy.i & 0x8000000000000000ULL;
    return ux.f;
}

static inline fs_float_t fs_scalbnl(fs_float_t x, int n)
{
    union { double f; uint64_t i; } u;
    double y = x;

    if (n > 1023) {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023)
                n = 1023;
        }
    } else if (n < -1022) {
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022)
                n = -1022;
        }
    }
    u.i = (uint64_t)(0x3ff + n) << 52;
    return y * u.f;
}

/* musl src/math/fmod.c (IEEE 754 binary64) */
static inline fs_float_t fs_fmodl(fs_float_t x, fs_float_t y)
{
    union { double f; uint64_t i; } ux = {x}, uy = {y};
    int ex = (int)(ux.i >> 52) & 0x7ff;
    int ey = (int)(uy.i >> 52) & 0x7ff;
    int sx = (int)(ux.i >> 63);
    uint64_t i;
    uint64_t uxi = ux.i;

    if (uy.i << 1 == 0 || y != y || ex == 0x7ff)
        return (x * y) / (x * y);
    if (uxi << 1 <= uy.i << 1) {
        if (uxi << 1 == uy.i << 1)
            return 0 * x;
        return x;
    }

    if (!ex) {
        for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1);
        uxi <<= -ex + 1;
    } else {
        uxi &= (uint64_t)-1 >> 12;
        uxi |= (uint64_t)1 << 52;
    }
    if (!ey) {
        for (i = uy.i << 12; i >> 63 == 0; ey--, i <<= 1);
        uy.i <<= -ey + 1;
    } else {
        uy.i &= (uint64_t)-1 >> 12;
        uy.i |= (uint64_t)1 << 52;
    }

    for (; ex > ey; ex--) {
        i = uxi - uy.i;
        if (i >> 63 == 0) {
            if (i == 0)
                return 0 * x;
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uy.i;
    if (i >> 63 == 0) {
        if (i == 0)
            return 0 * x;
        uxi = i;
    }
    for (; uxi >> 52 == 0; uxi <<= 1, ex--);

    if (ex > 0) {
        uxi -= (uint64_t)1 << 52;
        uxi |= (uint64_t)ex << 52;
    } else {
        uxi >>= -ex + 1;
    }
    uxi |= (uint64_t)sx << 63;
    ux.i = uxi;
    return ux.f;
}

static inline fs_float_t fs_mk_nan(int sign, const char *payload)
{
    union { double f; uint64_t i; } u;
    u.i = 0x7ff8000000000000ULL;
    if (payload) {
        uint64_t m = 0;
        int any = 0;
        for (; *payload; payload++) {
            if (*payload < '0' || *payload > '9') break;
            m = m * 10 + (uint64_t)(*payload - '0');
            any = 1;
        }
        if (any)
            u.i |= m & 0x0007ffffffffffffULL;
    }
    if (sign < 0)
        u.i |= 0x8000000000000000ULL;
    return u.f;
}

static inline float fs_narrow_nanf(fs_float_t y)
{
    union { double f; uint64_t i; } u64 = {y};
    union { float f; uint32_t i; } r;
    r.i = 0x7fc00000u | (uint32_t)(u64.i & 0x7fffffu);
    if (u64.i & 0x8000000000000000ULL) r.i |= 0x80000000u;
    return r.f;
}

static inline double fs_narrow_nand(fs_float_t y)
{
    return y;
}

static inline float fs_to_float(fs_float_t y) { return (float)y; }
static inline double fs_to_double(fs_float_t y) { return y; }

/* Pure-integer IEEE 754 binary64 -> binary128 (little-endian) encoding.
 * Preserves sign, normal, subnormal (renormalized to binary128 normal),
 * zero, infinity, and NaN payload without calling __extenddftf2. */
static inline long double fs_to_ldbl(fs_float_t y)
{
    union { double f; uint64_t u; } x = {y};
    union {
        long double f;
        struct {
            uint64_t lo;
            uint64_t hi;
        } u;
    } r;

    uint64_t sign = x.u & 0x8000000000000000ULL;
    uint64_t exp  = (x.u >> 52) & 0x7ffULL;
    uint64_t frac = x.u & 0x000fffffffffffffULL;

    if (exp == 0) {
        if (frac == 0) {
            r.u.hi = sign;
            r.u.lo = 0;
        } else {
            int shift = __builtin_clzll(frac) - 11;
            frac <<= shift;
            uint64_t e128 = (16383ULL - 1022ULL) - (uint64_t)shift;
            frac &= 0x000fffffffffffffULL;
            r.u.hi = sign | (e128 << 48) | (frac >> 4);
            r.u.lo = frac << 60;
        }
    } else if (exp == 0x7ffULL) {
        r.u.hi = sign | (0x7fffULL << 48) | (frac >> 4);
        r.u.lo = frac << 60;
    } else {
        uint64_t e128 = exp + (16383ULL - 1023ULL);
        r.u.hi = sign | (e128 << 48) | (frac >> 4);
        r.u.lo = frac << 60;
    }
    return r.f;
}

#endif /* _LIBC_ARCH_AARCH64_FLOAT_ABI_H */
