/* user/systest/test_libm.c — the sixteen libm symbols, inside the guest.
 *
 * The host test compares the implementation against the host's glibc,
 * but that only proves the arithmetic.  This case proves the other
 * half: that usr/lib/libm.a is really the archive the userland links
 * against, that the sixteen symbols resolve there, and that they keep
 * the error contract inside a running guest.
 *
 * Every expectation is a BIT PATTERN.  The inputs are built from bit
 * patterns too, so the compiler never rounds a literal and a signed
 * zero, a subnormal or a NaN payload reaches the function exactly.
 * The comparisons are integer comparisons: the guest never has to
 * evaluate a floating-point expression to judge the result.
 *
 * The calls go through volatile function pointers.  The compiler
 * cannot fold an argument into a builtin it knows, and the reference
 * to the archive symbol survives in the object file: test_libm.o
 * carries an undefined reference to each of the sixteen symbols.
 */

#include "systest.h"

#include <math.h>
#include <string.h>

/* ── bit helpers ────────────────────────────────────────────────────── */

static double d_from_bits(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static float f_from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static uint64_t d_to_bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static uint32_t f_to_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* The fixed quiet NaN the invalid paths must return. */
#define LIBM_QNAN_D 0x7ff8000000000000ULL
#define LIBM_QNAN_F 0x7fc00000U

/* The sentinel the error state is reset to before every call: a path
 * that must not report an error has to leave it untouched. */
#define LIBM_ERRNO_SENTINEL 0x5a5a

/* ── the calls go through volatile pointers ─────────────────────────── */

typedef double (*d1_t)(double);
typedef double (*d2_t)(double, double);
typedef float  (*f1_t)(float);
typedef float  (*f2_t)(float, float);

static volatile d1_t v_fabs     = fabs;
static volatile d1_t v_floor    = floor;
static volatile d1_t v_ceil     = ceil;
static volatile d1_t v_trunc    = trunc;
static volatile d1_t v_round    = round;
static volatile d1_t v_sqrt     = sqrt;
static volatile d2_t v_copysign = copysign;
static volatile d2_t v_fmod     = fmod;

static volatile f1_t v_fabsf     = fabsf;
static volatile f1_t v_floorf    = floorf;
static volatile f1_t v_ceilf     = ceilf;
static volatile f1_t v_truncf    = truncf;
static volatile f1_t v_roundf    = roundf;
static volatile f1_t v_sqrtf     = sqrtf;
static volatile f2_t v_copysignf = copysignf;
static volatile f2_t v_fmodf     = fmodf;

/* ── one expectation: the bit pattern and the error state ──────────── */

#define LIBM_CHECK_D(call, want_bits, want_errno, name) do {                  \
    errno = LIBM_ERRNO_SENTINEL;                                              \
    double got_ = (call);                                                     \
    uint64_t bits_ = d_to_bits(got_);                                         \
    int err_ = errno;                                                         \
    char det_[112];                                                           \
    snprintf(det_, sizeof(det_), "got %016llx errno %d",                      \
             (unsigned long long)bits_, err_);                                 \
    CHECK3(bits_ == (uint64_t)(want_bits) && err_ == (want_errno),            \
           (name), det_);                                                     \
} while (0)

#define LIBM_CHECK_F(call, want_bits, want_errno, name) do {                  \
    errno = LIBM_ERRNO_SENTINEL;                                              \
    float got_ = (call);                                                      \
    uint32_t bits_ = f_to_bits(got_);                                         \
    int err_ = errno;                                                         \
    char det_[112];                                                           \
    snprintf(det_, sizeof(det_), "got %08x errno %d",                         \
             (unsigned)bits_, err_);                                          \
    CHECK3(bits_ == (uint32_t)(want_bits) && err_ == (want_errno),            \
           (name), det_);                                                     \
} while (0)

/* ── 64_libm ────────────────────────────────────────────────────────── */

void test_libm(void)
{
    /* The ordinary vectors: one per function, both precisions. */
    LIBM_CHECK_D(v_fabs(d_from_bits(0x8000000000000000ULL)),
                 0x0000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm fabs -0");
    LIBM_CHECK_F(v_fabsf(f_from_bits(0x80000000U)),
                 0x00000000U, LIBM_ERRNO_SENTINEL, "libm fabsf -0");

    LIBM_CHECK_D(v_copysign(d_from_bits(0x3ff8000000000000ULL),
                            d_from_bits(0xbff0000000000000ULL)),
                 0xbff8000000000000ULL, LIBM_ERRNO_SENTINEL, "libm copysign 1.5 -1");
    LIBM_CHECK_F(v_copysignf(f_from_bits(0x3fc00000U),
                             f_from_bits(0xbf800000U)),
                 0xbfc00000U, LIBM_ERRNO_SENTINEL, "libm copysignf 1.5 -1");

    LIBM_CHECK_D(v_floor(d_from_bits(0xbff8000000000000ULL)),
                 0xc000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm floor -1.5");
    LIBM_CHECK_F(v_floorf(f_from_bits(0xbfc00000U)),
                 0xc0000000U, LIBM_ERRNO_SENTINEL, "libm floorf -1.5");

    LIBM_CHECK_D(v_ceil(d_from_bits(0x3ff8000000000000ULL)),
                 0x4000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm ceil 1.5");
    LIBM_CHECK_F(v_ceilf(f_from_bits(0x3fc00000U)),
                 0x40000000U, LIBM_ERRNO_SENTINEL, "libm ceilf 1.5");

    LIBM_CHECK_D(v_trunc(d_from_bits(0xbff8000000000000ULL)),
                 0xbff0000000000000ULL, LIBM_ERRNO_SENTINEL, "libm trunc -1.5");
    LIBM_CHECK_F(v_truncf(f_from_bits(0xbfc00000U)),
                 0xbf800000U, LIBM_ERRNO_SENTINEL, "libm truncf -1.5");

    LIBM_CHECK_D(v_round(d_from_bits(0x4004000000000000ULL)),
                 0x4008000000000000ULL, LIBM_ERRNO_SENTINEL, "libm round 2.5");
    LIBM_CHECK_F(v_roundf(f_from_bits(0x40200000U)),
                 0x40400000U, LIBM_ERRNO_SENTINEL, "libm roundf 2.5");

    LIBM_CHECK_D(v_fmod(d_from_bits(0x4016000000000000ULL),
                         d_from_bits(0x4000000000000000ULL)),
                 0x3ff8000000000000ULL, LIBM_ERRNO_SENTINEL, "libm fmod 5.5 2");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x40b00000U), f_from_bits(0x40000000U)),
                 0x3fc00000U, LIBM_ERRNO_SENTINEL, "libm fmodf 5.5 2");

    LIBM_CHECK_D(v_sqrt(d_from_bits(0x4010000000000000ULL)),
                 0x4000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm sqrt 4");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0x40800000U)),
                 0x40000000U, LIBM_ERRNO_SENTINEL, "libm sqrtf 4");

    /* The negative zero survives: a rounding function must not turn
     * -0 into +0, and the sign of a zero is a value of its own. */
    LIBM_CHECK_D(v_floor(d_from_bits(0x8000000000000000ULL)),
                 0x8000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm floor -0 keeps the sign");
    LIBM_CHECK_F(v_floorf(f_from_bits(0x80000000U)),
                 0x80000000U, LIBM_ERRNO_SENTINEL, "libm floorf -0 keeps the sign");
    LIBM_CHECK_D(v_ceil(d_from_bits(0x8000000000000000ULL)),
                 0x8000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm ceil -0 keeps the sign");
    LIBM_CHECK_F(v_ceilf(f_from_bits(0x80000000U)),
                 0x80000000U, LIBM_ERRNO_SENTINEL, "libm ceilf -0 keeps the sign");
    LIBM_CHECK_D(v_trunc(d_from_bits(0x8000000000000000ULL)),
                 0x8000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm trunc -0 keeps the sign");
    LIBM_CHECK_F(v_truncf(f_from_bits(0x80000000U)),
                 0x80000000U, LIBM_ERRNO_SENTINEL, "libm truncf -0 keeps the sign");
    LIBM_CHECK_D(v_round(d_from_bits(0x8000000000000000ULL)),
                 0x8000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm round -0 keeps the sign");
    LIBM_CHECK_F(v_roundf(f_from_bits(0x80000000U)),
                 0x80000000U, LIBM_ERRNO_SENTINEL, "libm roundf -0 keeps the sign");
    LIBM_CHECK_D(v_sqrt(d_from_bits(0x8000000000000000ULL)),
                 0x8000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm sqrt -0 keeps the sign");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0x80000000U)),
                 0x80000000U, LIBM_ERRNO_SENTINEL, "libm sqrtf -0 keeps the sign");
    LIBM_CHECK_D(v_fmod(d_from_bits(0x8000000000000000ULL),
                         d_from_bits(0x4000000000000000ULL)),
                 0x8000000000000000ULL, LIBM_ERRNO_SENTINEL, "libm fmod -0 2 keeps the sign");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x80000000U), f_from_bits(0x40000000U)),
                 0x80000000U, LIBM_ERRNO_SENTINEL, "libm fmodf -0 2 keeps the sign");

    /* The positive infinity passes through every rounding function. */
    LIBM_CHECK_D(v_floor(d_from_bits(0x7ff0000000000000ULL)),
                 0x7ff0000000000000ULL, LIBM_ERRNO_SENTINEL, "libm floor +inf");
    LIBM_CHECK_F(v_floorf(f_from_bits(0x7f800000U)),
                 0x7f800000U, LIBM_ERRNO_SENTINEL, "libm floorf +inf");
    LIBM_CHECK_D(v_ceil(d_from_bits(0x7ff0000000000000ULL)),
                 0x7ff0000000000000ULL, LIBM_ERRNO_SENTINEL, "libm ceil +inf");
    LIBM_CHECK_F(v_ceilf(f_from_bits(0x7f800000U)),
                 0x7f800000U, LIBM_ERRNO_SENTINEL, "libm ceilf +inf");
    LIBM_CHECK_D(v_trunc(d_from_bits(0x7ff0000000000000ULL)),
                 0x7ff0000000000000ULL, LIBM_ERRNO_SENTINEL, "libm trunc +inf");
    LIBM_CHECK_F(v_truncf(f_from_bits(0x7f800000U)),
                 0x7f800000U, LIBM_ERRNO_SENTINEL, "libm truncf +inf");
    LIBM_CHECK_D(v_round(d_from_bits(0x7ff0000000000000ULL)),
                 0x7ff0000000000000ULL, LIBM_ERRNO_SENTINEL, "libm round +inf");
    LIBM_CHECK_F(v_roundf(f_from_bits(0x7f800000U)),
                 0x7f800000U, LIBM_ERRNO_SENTINEL, "libm roundf +inf");
    LIBM_CHECK_D(v_sqrt(d_from_bits(0x7ff0000000000000ULL)),
                 0x7ff0000000000000ULL, LIBM_ERRNO_SENTINEL, "libm sqrt +inf");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0x7f800000U)),
                 0x7f800000U, LIBM_ERRNO_SENTINEL, "libm sqrtf +inf");
    /* A finite dividend divided by infinity is the dividend. */
    LIBM_CHECK_D(v_fmod(d_from_bits(0x4016000000000000ULL),
                         d_from_bits(0x7ff0000000000000ULL)),
                 0x4016000000000000ULL, LIBM_ERRNO_SENTINEL, "libm fmod 5.5 +inf");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x40b00000U), f_from_bits(0x7f800000U)),
                 0x40b00000U, LIBM_ERRNO_SENTINEL, "libm fmodf 5.5 +inf");

    /* fabs and copysign only touch the sign bit: the payload of a NaN
     * survives, and a negative NaN becomes a positive one. */
    LIBM_CHECK_D(v_fabs(d_from_bits(0x7ff8000000001234ULL)),
                 0x7ff8000000001234ULL, LIBM_ERRNO_SENTINEL, "libm fabs keeps the NaN payload");
    LIBM_CHECK_F(v_fabsf(f_from_bits(0x7fc00001U)),
                 0x7fc00001U, LIBM_ERRNO_SENTINEL, "libm fabsf keeps the NaN payload");
    LIBM_CHECK_D(v_fabs(d_from_bits(0xfff8000000001234ULL)),
                 0x7ff8000000001234ULL, LIBM_ERRNO_SENTINEL, "libm fabs clears the NaN sign");
    LIBM_CHECK_F(v_fabsf(f_from_bits(0xffc00001U)),
                 0x7fc00001U, LIBM_ERRNO_SENTINEL, "libm fabsf clears the NaN sign");
    LIBM_CHECK_D(v_copysign(d_from_bits(0x7ff8000000001234ULL),
                             d_from_bits(0xbff0000000000000ULL)),
                 0xfff8000000001234ULL, LIBM_ERRNO_SENTINEL, "libm copysign keeps the NaN payload");
    LIBM_CHECK_F(v_copysignf(f_from_bits(0x7fc00001U), f_from_bits(0xbf800000U)),
                 0xffc00001U, LIBM_ERRNO_SENTINEL, "libm copysignf keeps the NaN payload");
    LIBM_CHECK_D(v_copysign(d_from_bits(0xfff8000000001234ULL),
                             d_from_bits(0x3ff0000000000000ULL)),
                 0x7ff8000000001234ULL, LIBM_ERRNO_SENTINEL, "libm copysign applies the sign");
    LIBM_CHECK_F(v_copysignf(f_from_bits(0xffc00001U), f_from_bits(0x3f800000U)),
                 0x7fc00001U, LIBM_ERRNO_SENTINEL, "libm copysignf applies the sign");

    /* ── the domain errors ────────────────────────────────────────────
     * A negative argument to sqrt and a zero divisor or an infinite
     * dividend to fmod are domain errors: the result is the fixed
     * quiet NaN and the error state becomes EDOM. */
    LIBM_CHECK_D(v_sqrt(d_from_bits(0xbff0000000000000ULL)),
                 LIBM_QNAN_D, EDOM, "libm sqrt -1 reports EDOM");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0xbf800000U)),
                 LIBM_QNAN_F, EDOM, "libm sqrtf -1 reports EDOM");
    LIBM_CHECK_D(v_sqrt(d_from_bits(0xfff0000000000000ULL)),
                 LIBM_QNAN_D, EDOM, "libm sqrt -inf reports EDOM");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0xff800000U)),
                 LIBM_QNAN_F, EDOM, "libm sqrtf -inf reports EDOM");
    LIBM_CHECK_D(v_fmod(d_from_bits(0x3ff0000000000000ULL),
                         d_from_bits(0x0000000000000000ULL)),
                 LIBM_QNAN_D, EDOM, "libm fmod 1 0 reports EDOM");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x3f800000U), f_from_bits(0x00000000U)),
                 LIBM_QNAN_F, EDOM, "libm fmodf 1 0 reports EDOM");
    LIBM_CHECK_D(v_fmod(d_from_bits(0x7ff0000000000000ULL),
                         d_from_bits(0x4000000000000000ULL)),
                 LIBM_QNAN_D, EDOM, "libm fmod inf 2 reports EDOM");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x7f800000U), f_from_bits(0x40000000U)),
                 LIBM_QNAN_F, EDOM, "libm fmodf inf 2 reports EDOM");
    LIBM_CHECK_D(v_fmod(d_from_bits(0xfff0000000000000ULL),
                         d_from_bits(0x4000000000000000ULL)),
                 LIBM_QNAN_D, EDOM, "libm fmod -inf 2 reports EDOM");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0xff800000U), f_from_bits(0x40000000U)),
                 LIBM_QNAN_F, EDOM, "libm fmodf -inf 2 reports EDOM");
    LIBM_CHECK_D(v_fmod(d_from_bits(0x7ff0000000000000ULL),
                         d_from_bits(0x7ff0000000000000ULL)),
                 LIBM_QNAN_D, EDOM, "libm fmod inf inf reports EDOM");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x7f800000U), f_from_bits(0x7f800000U)),
                 LIBM_QNAN_F, EDOM, "libm fmodf inf inf reports EDOM");

    /* ── a NaN argument comes first ───────────────────────────────────
     * A NaN is not a domain error: the result is the fixed quiet NaN
     * and the error state stays untouched, even where the other
     * argument would have been a domain error. */
    LIBM_CHECK_D(v_sqrt(d_from_bits(0xfff8000000001234ULL)),
                 LIBM_QNAN_D, LIBM_ERRNO_SENTINEL, "libm sqrt -NaN keeps the sentinel");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0xffc00001U)),
                 LIBM_QNAN_F, LIBM_ERRNO_SENTINEL, "libm sqrtf -NaN keeps the sentinel");
    LIBM_CHECK_D(v_fmod(d_from_bits(0x7ff8000000001234ULL),
                         d_from_bits(0x0000000000000000ULL)),
                 LIBM_QNAN_D, LIBM_ERRNO_SENTINEL, "libm fmod NaN 0 keeps the sentinel");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x7fc00001U), f_from_bits(0x00000000U)),
                 LIBM_QNAN_F, LIBM_ERRNO_SENTINEL, "libm fmodf NaN 0 keeps the sentinel");
    LIBM_CHECK_D(v_fmod(d_from_bits(0x7ff0000000000000ULL),
                         d_from_bits(0x7ff8000000001234ULL)),
                 LIBM_QNAN_D, LIBM_ERRNO_SENTINEL, "libm fmod inf NaN keeps the sentinel");
    LIBM_CHECK_F(v_fmodf(f_from_bits(0x7f800000U), f_from_bits(0x7fc00001U)),
                 LIBM_QNAN_F, LIBM_ERRNO_SENTINEL, "libm fmodf inf NaN keeps the sentinel");

    /* A signalling NaN is a NaN as well: it must not raise EDOM. */
    LIBM_CHECK_D(v_sqrt(d_from_bits(0x7ff0000000000001ULL)),
                 LIBM_QNAN_D, LIBM_ERRNO_SENTINEL, "libm sqrt sNaN keeps the sentinel");
    LIBM_CHECK_F(v_sqrtf(f_from_bits(0x7f800001U)),
                 LIBM_QNAN_F, LIBM_ERRNO_SENTINEL, "libm sqrtf sNaN keeps the sentinel");
}
