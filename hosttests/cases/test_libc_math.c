/* @os01-test
 * id: test_libc_math
 * layer: host
 * subsystem: libc
 * timing: strict
 * tier: nightly
 * link: libc-math
 * scenarios: []
 */
/*
 * test_libc_math.c — the public math.h contract.
 *
 * The header is the contract between the caller and the standalone
 * usr/lib/libm.a: it declares the sixteen functions and it classifies
 * its arguments.  This file checks the classification half:
 *
 *  - the macros must keep accepting INTEGER arguments (the previous
 *    header converted them to double implicitly, and isfinite(int) /
 *    signbit(int) are used by existing callers);
 *  - they must classify the negative zero, a float subnormal, LDBL_MAX,
 *    a NaN and the infinities correctly;
 *  - they must evaluate a side-effecting argument exactly once.
 *
 * The numeric behaviour of the sixteen functions is checked against the
 * host glibc oracle in the glibc-oracle task; the C linkage of the
 * declarations is checked by the C++ probe
 * (hosttests/mock/libm_cxx_probe.cpp), whose link fails if the header
 * drops the extern "C" wrapper.
 */

#include "test_framework.h"

#include <math.h>

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The OS01 math objects report the error state through this global.
 * The host's errno is a TLS symbol and cannot be linked next to a plain
 * global ("TLS definition ... mismatches non-TLS reference"), so the
 * host build renames it (see the -Derrno=os01_errno flag in
 * hosttests/mk/libc.mk). */
int os01_errno;

static int side_effects;

static double bump(double v)
{
    side_effects++;
    return v;
}

TEST_FUNC(test_classification_accepts_integers)
{
    /* the previous header accepted these through the implicit conversion
     * to double; the new one must not regress */
    assert_eq(1, isfinite(5));
    assert_eq(1, isnormal(5));
    assert_eq(1, !isnan(5));
    assert_eq(1, !isinf(5));
    assert_eq(0, signbit(1));
    assert_eq(1, signbit(-1));
    assert_eq(1, fpclassify(5) == FP_NORMAL);
}

TEST_FUNC(test_classification_values)
{
    double qnan = __builtin_nan("");
    double inf = __builtin_inf();

    assert_eq(1, !isnan(0.0));
    assert_eq(1, isnan(qnan));
    assert_eq(1, isinf(inf));
    assert_eq(1, !isfinite(inf));
    assert_eq(1, isfinite(0.0));
    assert_eq(1, signbit(-0.0));
    assert_eq(0, signbit(0.0));

    /* the smallest positive float subnormal is finite but subnormal */
    float tiny = 1e-45f;
    assert_eq(1, isfinite(tiny));
    assert_eq(1, !isnormal(tiny));
    assert_eq(FP_SUBNORMAL, fpclassify(tiny));
    assert_eq(FP_NORMAL, fpclassify(1.0f));
    assert_eq(FP_ZERO, fpclassify(0.0));
    assert_eq(FP_INFINITE, fpclassify(inf));
    assert_eq(FP_NAN, fpclassify(qnan));
}

TEST_FUNC(test_classification_long_double)
{
    long double lmax = LDBL_MAX;
    long double linf = __builtin_infl();
    long double lnan = __builtin_nanl("");

    assert_eq(1, isfinite(lmax));
    assert_eq(1, isnormal(lmax));
    assert_eq(1, isinf(linf));
    assert_eq(1, !isfinite(linf));
    assert_eq(1, isnan(lnan));
    assert_eq(1, !isnan(lmax));
    assert_eq(1, signbit(-linf));
    assert_eq(0, signbit(linf));
    assert_eq(FP_INFINITE, fpclassify(linf));
    assert_eq(FP_NAN, fpclassify(lnan));
    assert_eq(FP_NORMAL, fpclassify(lmax));
}

TEST_FUNC(test_classification_evaluates_once)
{
    side_effects = 0;
    (void)isfinite(bump(1.0));
    assert_eq(1, side_effects);

    side_effects = 0;
    (void)isnan(bump(1.0));
    assert_eq(1, side_effects);

    side_effects = 0;
    (void)signbit(bump(1.0));
    assert_eq(1, side_effects);

    side_effects = 0;
    (void)fpclassify(bump(1.0));
    assert_eq(1, side_effects);
}

TEST_FUNC(test_cxx_linkage_probe)
{
    /* The probe is a freestanding C++ translation unit that calls fabs
     * through the public header.  It is compiled with the same rename
     * as the production objects, so the link succeeds only when the
     * header gives the declaration C linkage (without it the reference
     * would be the mangled _Z9os01_fabsd). */
    extern double os01_cxx_probe_fabs(double);
    assert_true(os01_cxx_probe_fabs(-3.5) == 3.5);
    assert_true(os01_cxx_probe_fabs(3.5) == 3.5);

    /* the same classification contract, checked in C++ (overload
     * dispatch instead of _Generic) */
    extern int os01_cxx_probe_classify(void);
    assert_eq(0, os01_cxx_probe_classify());
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_classification_accepts_integers),
    TEST_ENTRY(test_classification_values),
    TEST_ENTRY(test_classification_long_double),
    TEST_ENTRY(test_classification_evaluates_once),
    TEST_ENTRY(test_cxx_linkage_probe),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
