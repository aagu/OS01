#ifndef _LIBC_ARCH_X86_64_FLOAT_ABI_H
#define _LIBC_ARCH_X86_64_FLOAT_ABI_H

#include <float.h>
#include <stdint.h>

#if LDBL_MANT_DIG != 64 || LDBL_MAX_EXP != 16384
#error "x86_64 float_abi requires the x87 80-bit long double ABI"
#endif

typedef long double fs_float_t;

#define FS_MANT_DIG   LDBL_MANT_DIG
#define FS_MAX_EXP    LDBL_MAX_EXP
#define FS_MIN_EXP    LDBL_MIN_EXP
#define FS_MAX        LDBL_MAX
#define FS_MIN        LDBL_MIN
#define FS_EPSILON    LDBL_EPSILON
#define FS_LDBL_MIN   LDBL_MIN

union fs_ldshape {
    long double f;
    struct {
        uint64_t m;
        uint16_t se;
    } i;
};

static inline fs_float_t fs_fabs(fs_float_t x)
{
    return __builtin_fabsl(x);
}

static inline fs_float_t fs_copysignl(fs_float_t x, fs_float_t y)
{
    union fs_ldshape ux = {x}, uy = {y};
    ux.i.se &= 0x7fff;
    ux.i.se |= uy.i.se & 0x8000;
    return ux.f;
}

static inline fs_float_t fs_scalbnl(fs_float_t x, int n)
{
    union fs_ldshape u;

    if (n > 16383) {
        x *= 0x1p16383L;
        n -= 16383;
        if (n > 16383) {
            x *= 0x1p16383L;
            n -= 16383;
            if (n > 16383)
                n = 16383;
        }
    } else if (n < -16382) {
        x *= 0x1p-16382L * 0x1p113L;
        n += 16382 - 113;
        if (n < -16382) {
            x *= 0x1p-16382L * 0x1p113L;
            n += 16382 - 113;
            if (n < -16382)
                n = -16382;
        }
    }
    u.f = 1.0;
    u.i.se = (uint16_t)(0x3fff + n);
    return x * u.f;
}

static inline fs_float_t fs_fmodl(fs_float_t x, fs_float_t y)
{
    union fs_ldshape ux = {x}, uy = {y};
    int ex = ux.i.se & 0x7fff;
    int ey = uy.i.se & 0x7fff;
    int sx = ux.i.se & 0x8000;

    if (y == 0 || y != y || ex == 0x7fff)
        return (x * y) / (x * y);
    ux.i.se = (uint16_t)ex;
    uy.i.se = (uint16_t)ey;
    if (ux.f <= uy.f) {
        if (ux.f == uy.f)
            return 0 * x;
        return x;
    }

    if (!ex) {
        ux.f *= 0x1p120f;
        ex = ux.i.se - 120;
    }
    if (!ey) {
        uy.f *= 0x1p120f;
        ey = uy.i.se - 120;
    }

    uint64_t i, mx, my;
    mx = ux.i.m;
    my = uy.i.m;
    for (; ex > ey; ex--) {
        i = mx - my;
        if (mx >= my) {
            if (i == 0)
                return 0 * x;
            mx = 2 * i;
        } else if (2 * mx < mx) {
            mx = 2 * mx - my;
        } else {
            mx = 2 * mx;
        }
    }
    i = mx - my;
    if (mx >= my) {
        if (i == 0)
            return 0 * x;
        mx = i;
    }
    for (; mx >> 63 == 0; mx *= 2, ex--);
    ux.i.m = mx;

    if (ex <= 0) {
        ux.i.se = (uint16_t)((ex + 120) | sx);
        ux.f *= 0x1p-120f;
    } else
        ux.i.se = (uint16_t)(ex | sx);
    return ux.f;
}

static inline fs_float_t fs_mk_nan(int sign, const char *payload)
{
    union fs_ldshape u;
    u.f = __builtin_nanf("");
    if (payload) {
        uint64_t m = 0;
        int any = 0;
        for (; *payload; payload++) {
            if (*payload < '0' || *payload > '9') break;
            m = m * 10 + (uint64_t)(*payload - '0');
            any = 1;
        }
        if (any)
            u.i.m |= m & (((uint64_t)1 << 62) - 1);
    }
    if (sign < 0)
        u.i.se |= 0x8000;
    return u.f;
}

static inline float fs_narrow_nanf(fs_float_t y)
{
    union fs_ldshape u80 = {y};
    union { float f; uint32_t i; } r;
    r.i = 0x7fc00000u | (uint32_t)(u80.i.m & 0x7fffffu);
    if (u80.i.se & 0x8000) r.i |= 0x80000000u;
    return r.f;
}

static inline double fs_narrow_nand(fs_float_t y)
{
    union fs_ldshape u80 = {y};
    union { double f; uint64_t i; } r;
    r.i = 0x7ff8000000000000ULL | (u80.i.m & 0xffffffffffffULL);
    if (u80.i.se & 0x8000) r.i |= 0x8000000000000000ULL;
    return r.f;
}

static inline float fs_to_float(fs_float_t y) { return (float)y; }
static inline double fs_to_double(fs_float_t y) { return (double)y; }
static inline long double fs_to_ldbl(fs_float_t y) { return y; }

#endif /* _LIBC_ARCH_X86_64_FLOAT_ABI_H */
