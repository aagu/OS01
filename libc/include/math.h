#ifndef LIBC_MATH_H
#define LIBC_MATH_H

#include <float.h>
#include <stdint.h>
#include <string.h>

/* ── constants ──────────────────────────────────────────────────────
 *
 * musl's spelling.  INFINITY and NAN use the SAME replacement lists as
 * <float.h>, so a translation unit that includes both headers does not
 * trip the "macro redefined" diagnostic.
 */
#define INFINITY      (__builtin_inff())
#define NAN           (__builtin_nanf(""))
#define HUGE_VAL      (__builtin_huge_val())
#define HUGE_VALF     (__builtin_huge_valf())

/* OS01 implements the errno half of math_errhandling only: the libc has
 * no fenv, so the FP exception flags are never raised. */
#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

/* The C99 classification results (the values glibc and musl use). */
#define FP_NAN        0
#define FP_INFINITE   1
#define FP_ZERO       2
#define FP_SUBNORMAL  3
#define FP_NORMAL     4

/* ── classification helpers (internal linkage) ─────────────────────
 *
 * float and double use the compiler's classification builtins.  long
 * double is classified by reading its object representation: OS01 never
 * performs long double arithmetic (x86_64 would need x87, aarch64
 * would need the compiler-rt soft-float helpers), so the layout is
 * read as bytes instead of being compared as a floating-point value.
 *
 * The helpers live OUTSIDE the extern "C" block on purpose: they are
 * internal-linkage helpers, and C++ overloads may not be declared with
 * C linkage.
 */

static inline int os01_isnan_f(float x)      { return __builtin_isnan(x); }
static inline int os01_isnan_d(double x)     { return __builtin_isnan(x); }
static inline int os01_isinf_f(float x)      { return __builtin_isinf(x); }
static inline int os01_isinf_d(double x)     { return __builtin_isinf(x); }
static inline int os01_isfinite_f(float x)   { return __builtin_isfinite(x); }
static inline int os01_isfinite_d(double x)  { return __builtin_isfinite(x); }
static inline int os01_isnormal_f(float x)   { return __builtin_isnormal(x); }
static inline int os01_isnormal_d(double x)  { return __builtin_isnormal(x); }

static inline int os01_fpclassify_f(float x)
{
    return __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL,
                                FP_ZERO, x);
}

static inline int os01_fpclassify_d(double x)
{
    return __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL,
                                FP_ZERO, x);
}

/* signbit reads the raw sign bit of the object representation, so it
 * answers for NaN and for the negative zero as well. */
static inline int os01_signbit_f(float x)
{
    union { float f; uint32_t i; } u;
    u.f = x;
    return (int)(u.i >> 31);
}

static inline int os01_signbit_d(double x)
{
    union { double f; uint64_t i; } u;
    u.f = x;
    return (int)(u.i >> 63);
}

#if defined(__x86_64__)
/* x87 80-bit extended, stored in 16 bytes: the 64-bit significand in
 * bytes 0-7, the sign and the 15-bit exponent in bytes 8-9, and six
 * padding bytes after them (skipped — they carry no value). */
static inline int os01_fpclassify_ld(long double x)
{
    unsigned char b[sizeof(long double)];
    uint64_t sig;
    uint16_t se;

    memcpy(b, &x, sizeof(b));
    memcpy(&sig, b, 8);
    memcpy(&se, b + 8, 2);

    unsigned exp = (unsigned)(se & 0x7fffu);
    if (exp == 0x7fffu)
        /* the x87 infinity has the explicit integer bit set; every other
         * exponent-all-ones encoding is a NaN */
        return sig == 0x8000000000000000ULL ? FP_INFINITE : FP_NAN;
    if (exp == 0)
        return sig ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

static inline int os01_signbit_ld(long double x)
{
    unsigned char b[sizeof(long double)];
    uint16_t se;

    memcpy(b, &x, sizeof(b));
    memcpy(&se, b + 8, 2);

    return (int)((se >> 15) & 1u);
}
#elif defined(__aarch64__)
/* binary128, little endian: the low word in bytes 0-7 and the high word
 * in bytes 8-15 (sign bit 63, exponent bits 62-48, the top significand
 * bits 47-0 of the high word). */
static inline int os01_fpclassify_ld(long double x)
{
    unsigned char b[sizeof(long double)];
    uint64_t lo, hi;

    memcpy(b, &x, sizeof(b));
    memcpy(&lo, b, 8);
    memcpy(&hi, b + 8, 8);

    unsigned exp = (unsigned)((hi >> 48) & 0x7fffu);
    uint64_t sig = lo | (hi & 0x0000ffffffffffffULL);
    if (exp == 0x7fffu)
        return sig ? FP_NAN : FP_INFINITE;
    if (exp == 0)
        return sig ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

static inline int os01_signbit_ld(long double x)
{
    unsigned char b[sizeof(long double)];
    uint64_t hi;

    memcpy(b, &x, sizeof(b));
    memcpy(&hi, b + 8, 8);

    return (int)(hi >> 63);
}
#else
#error "math.h: no long double object representation for this architecture"
#endif

static inline int os01_isnan_ld(long double x)
{
    return os01_fpclassify_ld(x) == FP_NAN;
}

static inline int os01_isinf_ld(long double x)
{
    return os01_fpclassify_ld(x) == FP_INFINITE;
}

static inline int os01_isfinite_ld(long double x)
{
    int c = os01_fpclassify_ld(x);
    return c != FP_NAN && c != FP_INFINITE;
}

static inline int os01_isnormal_ld(long double x)
{
    return os01_fpclassify_ld(x) == FP_NORMAL;
}

/* ── the sixteen public functions ───────────────────────────────────
 *
 * They live in the standalone usr/lib/libm.a (the vendored musl math
 * subset), NOT in libc.a and NOT in libk.a: the kernel stays FP-free.
 * A consumer that calls one of them must link -lm.
 */
#ifdef __cplusplus
extern "C" {
#endif

double      fabs(double);
float       fabsf(float);
double      copysign(double, double);
float       copysignf(float, float);
double      floor(double);
float       floorf(float);
double      ceil(double);
float       ceilf(float);
double      trunc(double);
float       truncf(float);
double      round(double);
float       roundf(float);
double      fmod(double, double);
float       fmodf(float, float);
double      sqrt(double);
float       sqrtf(float);

#ifdef __cplusplus
}
#endif

/* ── the classification macros ──────────────────────────────────────
 *
 * Each macro dispatches on the argument type and evaluates its
 * argument exactly once: in C the controlling expression of _Generic
 * is not evaluated, in C++ the selected overload is called once.
 *
 * An integer argument keeps working: it is converted to double and
 * classified there, which is what the previous header did through its
 * implicit conversion.
 */
#ifdef __cplusplus

/* C++ dispatches by overload, not by _Generic.  The non-template
 * overloads win for the three floating-point types; the template is
 * the integer fallback (it is only an exact match for a non-floating
 * type, and a non-template function beats a template specialization
 * when both are exact matches). */
static inline int os01_isnan_impl(float x)       { return os01_isnan_f(x); }
static inline int os01_isnan_impl(double x)      { return os01_isnan_d(x); }
static inline int os01_isnan_impl(long double x)  { return os01_isnan_ld(x); }
template <typename T>
static inline int os01_isnan_impl(T x)            { return os01_isnan_d((double)x); }

static inline int os01_isinf_impl(float x)       { return os01_isinf_f(x); }
static inline int os01_isinf_impl(double x)      { return os01_isinf_d(x); }
static inline int os01_isinf_impl(long double x)  { return os01_isinf_ld(x); }
template <typename T>
static inline int os01_isinf_impl(T x)            { return os01_isinf_d((double)x); }

static inline int os01_isfinite_impl(float x)      { return os01_isfinite_f(x); }
static inline int os01_isfinite_impl(double x)     { return os01_isfinite_d(x); }
static inline int os01_isfinite_impl(long double x) { return os01_isfinite_ld(x); }
template <typename T>
static inline int os01_isfinite_impl(T x)           { return os01_isfinite_d((double)x); }

static inline int os01_isnormal_impl(float x)      { return os01_isnormal_f(x); }
static inline int os01_isnormal_impl(double x)     { return os01_isnormal_d(x); }
static inline int os01_isnormal_impl(long double x) { return os01_isnormal_ld(x); }
template <typename T>
static inline int os01_isnormal_impl(T x)           { return os01_isnormal_d((double)x); }

static inline int os01_fpclassify_impl(float x)      { return os01_fpclassify_f(x); }
static inline int os01_fpclassify_impl(double x)     { return os01_fpclassify_d(x); }
static inline int os01_fpclassify_impl(long double x) { return os01_fpclassify_ld(x); }
template <typename T>
static inline int os01_fpclassify_impl(T x)           { return os01_fpclassify_d((double)x); }

static inline int os01_signbit_impl(float x)      { return os01_signbit_f(x); }
static inline int os01_signbit_impl(double x)     { return os01_signbit_d(x); }
static inline int os01_signbit_impl(long double x) { return os01_signbit_ld(x); }
template <typename T>
static inline int os01_signbit_impl(T x)           { return os01_signbit_d((double)x); }

#define isnan(x)      os01_isnan_impl(x)
#define isinf(x)      os01_isinf_impl(x)
#define isfinite(x)   os01_isfinite_impl(x)
#define isnormal(x)   os01_isnormal_impl(x)
#define fpclassify(x) os01_fpclassify_impl(x)
#define signbit(x)    os01_signbit_impl(x)

#else /* C */

#define isnan(x) _Generic((x), \
    float:      os01_isnan_f, \
    double:     os01_isnan_d, \
    long double: os01_isnan_ld, \
    default:    os01_isnan_d)(x)

#define isinf(x) _Generic((x), \
    float:      os01_isinf_f, \
    double:     os01_isinf_d, \
    long double: os01_isinf_ld, \
    default:    os01_isinf_d)(x)

#define isfinite(x) _Generic((x), \
    float:      os01_isfinite_f, \
    double:     os01_isfinite_d, \
    long double: os01_isfinite_ld, \
    default:    os01_isfinite_d)(x)

#define isnormal(x) _Generic((x), \
    float:      os01_isnormal_f, \
    double:     os01_isnormal_d, \
    long double: os01_isnormal_ld, \
    default:    os01_isnormal_d)(x)

#define fpclassify(x) _Generic((x), \
    float:      os01_fpclassify_f, \
    double:     os01_fpclassify_d, \
    long double: os01_fpclassify_ld, \
    default:    os01_fpclassify_d)(x)

#define signbit(x) _Generic((x), \
    float:      os01_signbit_f, \
    double:     os01_signbit_d, \
    long double: os01_signbit_ld, \
    default:    os01_signbit_d)(x)

#endif /* __cplusplus */

#endif /* LIBC_MATH_H */
