/*
 * test_libc_float_parse.c — libc-completion Task T11: precision-aware
 * floating-point parsing.
 *
 * Layer 1 — THE ORACLE TABLE: the shared table
 * (hosttests/data/floatscan_cases.h) is evaluated by the standalone
 * glibc oracle (hosttests/oracle/floatscan_oracle.c, a separate
 * process compiled with no OS01 include paths) and by THIS binary
 * against the OS01 floatscan port.  Both implementations are
 * correctly rounded, so the raw IEEE BIT PATTERNS (float 4 bytes,
 * double 8 bytes, the 10 significant bytes of the x87 80-bit long
 * double — padding skipped), the consumed length and the errno must
 * agree on every row.
 *
 * Layer 2 — LONG DOUBLE PRECISION: values needing more than double
 * precision must survive as long doubles (the old strtod could not
 * even hold them).  Checked directly against exact 80-bit patterns.
 *
 * Layer 3 — errno discipline: ERANGE on overflow/underflow, errno
 * preserved when nothing is consumed (the scanner's EINVAL on
 * no-conversion is suppressed by the wrappers).
 *
 * Layer 4 — the absolute value of a binary64: fabs() clears the sign
 * bit, so -0.0 becomes +0.0, the infinities lose their sign and a NaN
 * keeps its payload.  The header inline is renamed in this translation
 * unit, so the host libc's fabs can never answer for it, and a SECOND
 * translation unit (the same source compiled with -DOS01_FABS_PROBE_TU
 * and -fno-builtin) links against the same header.
 */

#include "test_framework.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../data/floatscan_cases.h"

#ifdef OS01_FABS_PROBE_TU

/* ── the second translation unit of the fabs probe ──────────────────
 *
 * The SAME source, compiled with -DOS01_FABS_PROBE_TU, carries nothing
 * but this function: it includes <math.h> and calls fabs() exactly like
 * the test translation unit does.  It is compiled with -fno-builtin, so
 * the compiler's builtin cannot answer the call either — the header's
 * implementation is the only possible answer.
 *
 * Two translation units, one header, one definition: the link proves
 * that the header carries no duplicate definition, and it proves that
 * the implementation really lives in the header. */
int os01_fabs_probe_value(void)
{
    return (fabs(-3.5) == 3.5 && fabs(-0.0) == 0.0) ? 42 : 0;
}

#else

/* baked in by the make rule; stays inside the profile build dir */
#ifndef FLOATSCAN_ORACLE_PATH
#define FLOATSCAN_ORACLE_PATH "floatscan_oracle-missing"
#endif

/* hex dump of a value's significant bytes (padding excluded) */
static void dump_hex(char *out, const void *v, int bytes)
{
    const unsigned char *p = v;
    for (int i = 0; i < bytes; i++)
        sprintf(out + 2 * i, "%02x", p[i]);
    out[2 * bytes] = '\0';
}

/* Run the row through OUR strtod/strtof/strtold and format the same
 * tuple shape the oracle prints. */
static void format_row(char *out, size_t cap, int i)
{
    const char *s = float_cases[i].input;

    errno = 0;
    float f = strtof(s, NULL);
    int ef = errno;

    errno = 0;
    double d = strtod(s, NULL);
    int ed = errno;

    char *e = NULL;
    errno = 0;
    long double ld = strtold(s, &e);
    int eld = errno;
    long off = e ? (long)(e - s) : -1;

    char hf[16], hd[32], hld[32];
    dump_hex(hf, &f, 4);
    dump_hex(hd, &d, 8);
    dump_hex(hld, &ld, 10);

    snprintf(out, cap,
             "%d\tf=%s/%d\td=%s/%d\tld=%s/%d\tof=%ld\n",
             i, hf, ef, hd, ed, hld, eld, off);
}

TEST_FUNC(test_oracle_bit_pattern_agreement) {
    static char out[16 * 1024];

    int fds[2];
    assert_eq(0, pipe(fds));
    int64_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *argv[2] = { FLOATSCAN_ORACLE_PATH, NULL };
        execv(FLOATSCAN_ORACLE_PATH, argv);
        _exit(127);
    }
    close(fds[1]);
    size_t total = 0;
    for (;;) {
        if (total + 1 >= sizeof(out)) break;
        ssize_t n = read(fds[0], out + total, sizeof(out) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fds[0]);
    out[total] = '\0';
    int st = -1;
    int64_t w = waitpid(pid, &st, 0);
    assert_true(w == pid && st == 0);
    assert_true(total > 0);

    char want[256], got[256];
    int mismatches = 0;
    const char *cursor = out;
    for (int i = 0; i < FLOAT_CASE_COUNT; i++) {
        format_row(got, sizeof(got), i);
        const char *nl = strchr(cursor, '\n');
        if (!nl) {
            printf("  [FAIL] oracle produced only %d of %d rows\n",
                   i, FLOAT_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor) + 1;
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        if (strcmp(want, got) != 0) {
            printf("  [FAIL] row %d (input=\"%s\")\n"
                   "         oracle: %s"
                   "         os01:   %s",
                   i, float_cases[i].input, want, got);
            mismatches++;
        }
    }
    if (*cursor != '\0') {
        printf("  [FAIL] oracle produced MORE rows than the table\n");
        mismatches++;
    }
    assert_eq(0, mismatches);
}

TEST_FUNC(test_long_double_keeps_extra_precision) {
    /* 35 significant digits: a double holds ~15.95, a long double
     * ~18.5 — the LONG DOUBLE must carry more of the tail than the
     * double does, and both must be exactly what glibc computes (the
     * oracle check pins them; here we pin the CONTRAST). */
    const char *s = "3.14159265358979323846264338327950288";
    double d = strtod(s, NULL);
    long double ld = strtold(s, NULL);

    /* long double is strictly finer than double here: widen the
     * double and the difference must be within half an LDBL ulp but
     * NONZERO at the LDBL_EPSILON scale. */
    long double dw = (long double)d;
    long double diff = ld - dw;
    assert_true(diff != 0.0L);           /* extra digits survived */
    /* the difference is bounded by the DOUBLE's rounding (half a
     * double ulp at π's scale), not by LDBL_EPSILON (a relative bound
     * at 1.0 — LDBL_EPSILON * 4 ≈ 4e-19 would be far too tight) */
    assert_true(__builtin_fabsl(diff) < DBL_EPSILON * 4.0);

    /* hex float exactness: 0x1.8p+1 is exactly 3.0 in every format */
    assert_eq(3.0, strtod("0x1.8p+1", NULL));
    assert_eq(3.0L, strtold("0x1.8p+1", NULL));

    /* LDBL boundary: LDBL_MAX's decimal prefix parses back to the
     * exact value.  The string is a CONSTANT — generating it with OUR
     * snprintf would test floatconv's %Le (which does not carry 21
     * digits yet), not the parser. */
    errno = 0;
    long double m = strtold("1.189731495357231765021e+4932", NULL);
    assert_eq(0, errno);
    assert_true(m == LDBL_MAX);
}

TEST_FUNC(test_errno_and_consumption_contract) {
    errno = 7;
    assert_eq(0, strtod("abc", NULL));
    assert_eq(7, errno);                 /* no conversion: preserved */

    errno = 7;
    assert_eq(1, (int)strtod("1x", NULL));
    assert_eq(7, errno);                 /* success: preserved */

    /* "1e+" consumes exactly ONE character (the 'e' is un-gotten) */
    errno = 0;
    char *e = NULL;
    (void)strtod("1e+", &e);
    assert_eq(1, (long)(e - "1e+"));
    assert_eq(0, errno);

    /* overflow: inf + ERANGE */
    errno = 0;
    double inf = strtod("1e400", &e);
    assert_eq(ERANGE, errno);
    assert_true(inf > DBL_MAX);
    assert_eq(5, (long)(e - "1e400"));

    /* underflow: 0 + ERANGE */
    errno = 0;
    double zero = strtod("1e-400", NULL);
    assert_eq(ERANGE, errno);
    assert_eq(0, (int)zero);

    /* nothing consumed: endptr = nptr */
    e = NULL;
    (void)strtod("abc", &e);
    assert_eq(0, (long)(e - "abc"));
}

/* ── the absolute value of a binary64 ─────────────────────────────── */

/* The bit contract of fabs(): it clears the SIGN BIT of the binary64
 * representation, and that is exactly the whole contract:
 *
 *   -0.0   → +0.0     (the value stays zero, the sign bit is cleared)
 *   -Inf   → +Inf
 *   NaN    → NaN      (the payload survives, the sign bit is cleared)
 *   -7.5   → +7.5
 *
 * The header inline is renamed to os01_fabs in this translation unit (see
 * the make rule), so the host libc's fabs can never answer for it: the
 * link would fail if <math.h> did not carry the implementation.  The
 * signbit()/isfinite() macros of the same header are used unchanged. */
TEST_FUNC(fabs_bit_contract) {
    union { double d; uint64_t u; } v;

    /* -0.0 → +0.0 */
    double nz = fabs(-0.0);
    assert_eq(0, (int)nz);
    assert_eq(0, signbit(nz));
    /* the input really was the negative zero */
    assert_eq(1, signbit(-0.0));

    /* -Inf → +Inf */
    double inf = fabs(-INFINITY);
    assert_true(inf == INFINITY);
    assert_eq(0, signbit(inf));
    assert_eq(1, signbit(-INFINITY));

    /* NaN: the payload survives, the sign bit is cleared.  The input is a
     * NEGATIVE quiet NaN with a payload of 1, so the result must be the
     * same NaN with the sign bit cleared. */
    v.u = 0xfff8000000000001ull;
    v.d = fabs(v.d);
    assert_true(v.u == 0x7ff8000000000001ull);
    if (v.u != 0x7ff8000000000001ull) {
        printf("  [FAIL] fabs(NaN) bits: got %016llx, want %016llx\n",
               (unsigned long long)v.u,
               (unsigned long long)0x7ff8000000000001ull);
    }
    /* and it is still a NaN */
    assert_eq(0, isfinite(v.d));

    /* ordinary magnitudes */
    assert_true(fabs(-7.5) == 7.5);
    assert_true(fabs(7.5) == 7.5);
    assert_true(fabs(-0.5) == 0.5);

    /* the second translation unit of the probe: it includes the same
     * header, calls the same function and is compiled with -fno-builtin.
     * It links, so the header carries exactly one definition. */
    extern int os01_fabs_probe_value(void);
    assert_eq(42, os01_fabs_probe_value());
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_oracle_bit_pattern_agreement),
    TEST_ENTRY(test_long_double_keeps_extra_precision),
    TEST_ENTRY(test_errno_and_consumption_contract),
    TEST_ENTRY(fabs_bit_contract),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

#endif /* OS01_FABS_PROBE_TU */
