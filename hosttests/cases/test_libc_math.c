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
 * The numeric behaviour of the sixteen functions is checked in T3
 * against the glibc oracle; the C linkage of the declarations is
 * checked by the C++ probe (hosttests/mock/libm_cxx_probe.cpp), whose
 * link fails if the header drops the extern "C" wrapper.
 */

#include "test_framework.h"

#include <math.h>

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../data/libm_cases.h"

#ifndef LIBM_ORACLE_PATH
#define LIBM_ORACLE_PATH "libm_oracle-missing"
#endif

/* The OS01 math objects report the error state through this global.
 * The host's errno is a TLS symbol and cannot be linked next to a plain
 * global ("TLS definition ... mismatches non-TLS reference"), so the
 * host build renames it (see the -Derrno=os01_errno flag in
 * hosttests/mk/libc.mk). */
int os01_errno;

/* The sentinel the error state is reset to before every call: a path
 * that must not report an error has to leave it untouched. */
#define LIBM_ERRNO_SENTINEL 0x5a5a

/* The fixed quiet NaN the invalid paths return. */
#define LIBM_QNAN_D 0x7ff8000000000000ULL
#define LIBM_QNAN_F 0x7fc00000U

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

/* ── the numeric part: the OS01 implementation against glibc ──────
 *
 * The standalone oracle (hosttests/oracle/libm_oracle.c) is a separate
 * host program built with no OS01 include paths: it can only use the
 * host's <math.h> and the host's libm.  It walks the shared case table
 * and prints one row per case.  This test compares the OS01 result
 * with the oracle's, field by field:
 *
 *   - the transcript itself must be intact (one well-formed row per
 *     case, the known id, the right width and function, no duplicate,
 *     no missing and no extra row);
 *   - the result must match per the case's comparison strategy;
 *   - the error state must match the case's explicit expectation.
 */

static double libm_bits_to_double(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static float libm_bits_to_float(uint64_t u)
{
    float f;
    uint32_t v = (uint32_t)u;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static uint64_t libm_double_to_bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static uint32_t libm_float_to_bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static int libm_bits_is_nan(uint64_t bits, int width)
{
    if (width == 64)
        return ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) &&
               (bits & 0x000fffffffffffffULL) != 0;
    uint32_t b = (uint32_t)bits;
    return ((b & 0x7f800000U) == 0x7f800000U) && (b & 0x007fffffU) != 0;
}

/* Call the OS01 implementation of the case's function.  Every symbol is
 * renamed (os01_*) by the build, so the host's glibc can never answer
 * for the OS01 implementation. */
static uint64_t libm_call_os01(const struct libm_case *c)
{
    os01_errno = LIBM_ERRNO_SENTINEL;
    if (c->width == 64) {
        double x = libm_bits_to_double(c->a);
        double y = libm_bits_to_double(c->b);
        double r;
        if (!strcmp(c->fn, "fabs"))           r = fabs(x);
        else if (!strcmp(c->fn, "copysign"))  r = copysign(x, y);
        else if (!strcmp(c->fn, "floor"))     r = floor(x);
        else if (!strcmp(c->fn, "ceil"))      r = ceil(x);
        else if (!strcmp(c->fn, "trunc"))     r = trunc(x);
        else if (!strcmp(c->fn, "round"))     r = round(x);
        else if (!strcmp(c->fn, "fmod"))      r = fmod(x, y);
        else if (!strcmp(c->fn, "sqrt"))      r = sqrt(x);
        else { printf("  [FAIL] unknown function %s\n", c->fn); return 0; }
        return libm_double_to_bits(r);
    } else {
        float x = libm_bits_to_float(c->a);
        float y = libm_bits_to_float(c->b);
        float r;
        if (!strcmp(c->fn, "fabsf"))           r = fabsf(x);
        else if (!strcmp(c->fn, "copysignf"))  r = copysignf(x, y);
        else if (!strcmp(c->fn, "floorf"))     r = floorf(x);
        else if (!strcmp(c->fn, "ceilf"))      r = ceilf(x);
        else if (!strcmp(c->fn, "truncf"))     r = truncf(x);
        else if (!strcmp(c->fn, "roundf"))     r = roundf(x);
        else if (!strcmp(c->fn, "fmodf"))      r = fmodf(x, y);
        else if (!strcmp(c->fn, "sqrtf"))      r = sqrtf(x);
        else { printf("  [FAIL] unknown function %s\n", c->fn); return 0; }
        return libm_float_to_bits(r);
    }
}

/* Check the oracle transcript against the case table AND compare every
 * case with the OS01 implementation.  Returns the number of problems;
 * with report != 0 every problem is printed. */
static int libm_check_transcript(const char *out, int report)
{
    static unsigned char seen[LIBM_CASE_COUNT];
    static unsigned long long oracle_bits[LIBM_CASE_COUNT];
    int problems = 0;
    int rows = 0;
    const char *cursor = out;

    memset(seen, 0, sizeof(seen));
    memset(oracle_bits, 0, sizeof(oracle_bits));

    while (*cursor) {
        char id[64], fn[16], line[256];
        int width = 0, err = 0;
        unsigned long long bits = 0;
        const char *nl = strchr(cursor, '\n');
        size_t len = nl ? (size_t)(nl - cursor) : strlen(cursor);
        int idx = -1;

        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, cursor, len);
        line[len] = '\0';

        if (sscanf(line, "%63s %15s %d %llx %d", id, fn, &width, &bits, &err) != 5) {
            if (report)
                printf("  [FAIL] malformed oracle row: %s\n", line);
            problems++;
        } else {
            for (int i = 0; i < LIBM_CASE_COUNT; i++) {
                if (!strcmp(libm_cases[i].id, id)) { idx = i; break; }
            }
            if (idx < 0) {
                if (report)
                    printf("  [FAIL] unknown case id in the oracle output: %s\n", id);
                problems++;
            } else if (seen[idx]) {
                if (report)
                    printf("  [FAIL] duplicate case id in the oracle output: %s\n", id);
                problems++;
            } else if (width != libm_cases[idx].width) {
                if (report)
                    printf("  [FAIL] %s: the oracle reports width %d, the case says %d\n",
                           id, width, libm_cases[idx].width);
                problems++;
            } else if (strcmp(fn, libm_cases[idx].fn)) {
                if (report)
                    printf("  [FAIL] %s: the oracle reports %s, the case says %s\n",
                           id, fn, libm_cases[idx].fn);
                problems++;
            } else {
                seen[idx] = 1;
                oracle_bits[idx] = bits;
            }
        }
        rows++;
        if (!nl)
            break;
        cursor = nl + 1;
    }

    for (int i = 0; i < LIBM_CASE_COUNT; i++) {
        const struct libm_case *c = &libm_cases[i];
        uint64_t os01_bits, want_fixed;
        int os01_err;

        if (!seen[i]) {
            if (report)
                printf("  [FAIL] the oracle output misses the case %s\n", c->id);
            problems++;
            continue;
        }

        os01_bits = libm_call_os01(c);
        os01_err = os01_errno;

        /* the error contract comes from the case table, not from the
         * oracle: a path that must not report an error has to leave the
         * sentinel untouched */
        if (c->expect_errno == 0) {
            if (os01_err != LIBM_ERRNO_SENTINEL) {
                if (report)
                    printf("  [FAIL] %s: errno changed to %d, it must stay untouched\n",
                           c->id, os01_err);
                problems++;
            }
        } else if (os01_err != c->expect_errno) {
            if (report)
                printf("  [FAIL] %s: errno is %d, it must be %d\n",
                       c->id, os01_err, c->expect_errno);
            problems++;
        }

        want_fixed = (c->width == 64) ? LIBM_QNAN_D : (uint64_t)LIBM_QNAN_F;
        switch (c->cmp) {
        case LIBM_CMP_EXACT:
            if (os01_bits != oracle_bits[i]) {
                if (report)
                    printf("  [FAIL] %s: os01 %016llx, glibc %016llx\n",
                           c->id, (unsigned long long)os01_bits,
                           (unsigned long long)oracle_bits[i]);
                problems++;
            }
            break;
        case LIBM_CMP_NAN:
            if (!libm_bits_is_nan(os01_bits, c->width) ||
                !libm_bits_is_nan(oracle_bits[i], c->width)) {
                if (report)
                    printf("  [FAIL] %s: os01 %016llx, glibc %016llx (both must be NaN)\n",
                           c->id, (unsigned long long)os01_bits,
                           (unsigned long long)oracle_bits[i]);
                problems++;
            }
            break;
        case LIBM_CMP_FIXED_NAN:
            if (os01_bits != want_fixed) {
                if (report)
                    printf("  [FAIL] %s: os01 %016llx, the fixed quiet NaN is %016llx\n",
                           c->id, (unsigned long long)os01_bits,
                           (unsigned long long)want_fixed);
                problems++;
            }
            break;
        default:
            if (report)
                printf("  [FAIL] %s: unknown comparison strategy %d\n",
                       c->id, c->cmp);
            problems++;
            break;
        }
    }

    if (rows != LIBM_CASE_COUNT) {
        if (report)
            printf("  [FAIL] the oracle produced %d rows, the table has %d\n",
                   rows, LIBM_CASE_COUNT);
        problems++;
    }
    return problems;
}

/* Run the oracle and return its transcript in out (NUL terminated). */
static int libm_run_oracle(char *out, size_t cap)
{
    int fds[2];
    size_t total = 0;
    int st = -1;
    pid_t pid, w;

    if (pipe(fds) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *argv[2] = { (char *)LIBM_ORACLE_PATH, NULL };
        execv(LIBM_ORACLE_PATH, argv);
        _exit(127);
    }
    close(fds[1]);
    for (;;) {
        ssize_t n;
        if (total + 1 >= cap)
            break;
        n = read(fds[0], out + total, cap - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fds[0]);
    out[total] = '\0';
    w = waitpid(pid, &st, 0);
    if (w != pid || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

TEST_FUNC(test_numeric_matches_glibc_oracle)
{
    static char out[1 << 18];

    int rc = libm_run_oracle(out, sizeof(out));
    if (rc != 0) {
        printf("  [FAIL] the glibc oracle did not run (exit %d)\n", rc);
        assert_true(0);
        return;
    }
    assert_true(out[0] != '\0');

    int problems = libm_check_transcript(out, 1);
    assert_eq(0, problems);
}

TEST_FUNC(test_oracle_transcript_checks_are_real)
{
    /* The transcript checker must reject a corrupted oracle.  Without
     * this the test could pass on a transcript that never carried the
     * reference values at all. */
    static char out[1 << 18];
    static char mutated[1 << 18];
    const char *nl;

    int rc = libm_run_oracle(out, sizeof(out));
    if (rc != 0) {
        printf("  [FAIL] the glibc oracle did not run (exit %d)\n", rc);
        assert_true(0);
        return;
    }
    assert_eq(0, libm_check_transcript(out, 0));

    /* a duplicated row */
    nl = strchr(out, '\n');
    if (!nl) {
        printf("  [FAIL] the oracle transcript has no row\n");
        assert_true(0);
        return;
    }
    size_t first = (size_t)(nl - out) + 1;
    memcpy(mutated, out, first);
    memcpy(mutated + first, out, first);
    memcpy(mutated + 2 * first, out + first, strlen(out + first) + 1);
    assert_true(libm_check_transcript(mutated, 0) > 0);

    /* a wrong width: the third token of the first row */
    nl = strchr(out, '\n');
    assert_true(nl != NULL);
    size_t len = (size_t)(nl - out);
    memcpy(mutated, out, len + 1);
    char *p = mutated;
    for (int i = 0; i < 2; i++) {
        p = strchr(p, ' ');
        assert_true(p != NULL);
        p++;
    }
    p[0] = '9';
    p[1] = '9';
    memcpy(mutated + len + 1, out + len + 1, strlen(out + len + 1) + 1);
    assert_true(libm_check_transcript(mutated, 0) > 0);

    /* a truncated transcript */
    size_t half = strlen(out) / 2;
    memcpy(mutated, out, half);
    mutated[half] = '\0';
    assert_true(libm_check_transcript(mutated, 0) > 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_classification_accepts_integers),
    TEST_ENTRY(test_classification_values),
    TEST_ENTRY(test_classification_long_double),
    TEST_ENTRY(test_classification_evaluates_once),
    TEST_ENTRY(test_cxx_linkage_probe),
    TEST_ENTRY(test_numeric_matches_glibc_oracle),
    TEST_ENTRY(test_oracle_transcript_checks_are_real),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
