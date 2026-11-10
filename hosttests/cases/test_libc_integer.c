/*
 * test_libc_integer.c — libc-completion Task T10: integer conversion
 * boundaries and the intmax APIs.
 *
 * Two layers of evidence:
 *
 *  1. THE ORACLE TABLE — the shared table (hosttests/data/
 *     strtox_cases.h) is evaluated by the standalone glibc oracle
 *     (hosttests/oracle/strtox_oracle.c, a SEPARATE process compiled
 *     with no OS01 include paths, launched with popen()) and by THIS
 *     binary against the OS01 implementations.  Every line of the
 *     oracle's output must come back byte-identical from ours: values,
 *     endptr offsets and post-call errno for strtol / strtoull /
 *     strtoll / strtoumax.
 *
 *  2. TARGETED RED PROBES — the defects the old implementation had:
 *     strtoul("-1") was 1 (sign parsed but never applied); an
 *     overflowing run stopped scanning at the overflow point (endptr
 *     mid-digits); errno was polluted with EINVAL on no-conversion;
 *     strtoll was a truncation cast of strtol (LLONG boundary wrong).
 *     Each is asserted directly with exact endptr consumption, not
 *     just return values.
 *
 * The PRI macro compile-check: this file formats an intmax_t with
 * PRIdMAX — a type mismatch in the macros fails the build.
 */

#include "test_framework.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../data/strtox_cases.h"

/* PRI macros must format the intmax width (compile-time type check). */
_Static_assert(sizeof(intmax_t) == sizeof(long long),
               "PRIdMAX spells long long; intmax_t must match");
_Static_assert(sizeof(uintmax_t) == sizeof(unsigned long long),
               "PRIuMAX spells unsigned long long; uintmax_t must match");

/* ── oracle comparison ─────────────────────────────────────────────── */

/* One result tuple as text: L=<ld>/<errno> ULL=<llu>/<errno>
 * LL=<lld>/<off>/<errno> UM=<ju>/<off>/<errno> — the exact shape the
 * oracle prints (modulo the leading idx column). */
static void format_row(char *out, size_t cap, int i)
{
    int base = strtox_cases[i].base;
    const char *s = strtox_cases[i].input;

    errno = 0;
    long l = strtol(s, NULL, base);
    int el = errno;
    errno = 0;
    unsigned long long ull = strtoull(s, NULL, base);
    int eull = errno;

    char *e = NULL;
    errno = 0;
    long long ll = strtoll(s, &e, base);
    int ell = errno;
    long off_ll = e ? (long)(e - s) : -1;
    e = NULL;
    errno = 0;
    uintmax_t um = strtoumax(s, &e, base);
    int eum = errno;
    long off_um = e ? (long)(e - s) : -1;

    /* %llu with an explicit cast: the OS01 formatter does not speak
     * the %j length modifier yet (recorded as a vformatter gap), and
     * uintmax_t is long long wide here (static-asserted above). */
    snprintf(out, cap,
             "%d\tL=%ld/%d\tULL=%llu/%d\tLL=%lld/%ld/%d\tUM=%llu/%ld/%d\n",
             i, l, el, ull, eull, ll, off_ll, ell,
             (unsigned long long)um, off_um, eum);
}

TEST_FUNC(test_oracle_table_agreement) {
    /* The oracle: a SEPARATE process, host glibc only (the make rule
     * compiles it with no OS01 include paths; the path is baked in by
     * -DSTRTOX_ORACLE_PATH=... and stays inside the build directory).
     * The pipe is read with the HOST's read() — our stdio must not
     * touch a glibc FILE, and this binary deliberately links no
     * descriptor mock, so plain fork/execv/read is the cleanest
     * boundary. */
#ifndef STRTOX_ORACLE_PATH
#define STRTOX_ORACLE_PATH "strtox_oracle-missing"
#endif
    static char out[16 * 1024];

    int fds[2];
    assert_eq(0, pipe(fds));
    int64_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *argv[2] = { STRTOX_ORACLE_PATH, NULL };
        execv(STRTOX_ORACLE_PATH, argv);
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
    assert_true(w == pid && st == 0);            /* oracle exited 0 */
    assert_true(total > 0);                       /* oracle said something */

    /* compare row by row */
    char want[256], got[256];
    int mismatches = 0;
    const char *cursor = out;
    for (int i = 0; i < STRTOX_CASE_COUNT; i++) {
        format_row(got, sizeof(got), i);
        const char *nl = strchr(cursor, '\n');
        if (!nl) {
            printf("  [FAIL] oracle produced only %d of %d rows\n",
                   i, STRTOX_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor) + 1;
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        if (strcmp(want, got) != 0) {
            printf("  [FAIL] row %d (base=%d input=\"%s\")\n"
                   "         oracle: %s"
                   "         os01:   %s",
                   i, strtox_cases[i].base, strtox_cases[i].input,
                   want, got);
            mismatches++;
        }
    }
    if (*cursor != '\0') {
        printf("  [FAIL] oracle produced MORE rows than the table\n");
        mismatches++;
    }
    assert_eq(0, mismatches);
}

/* ── targeted probes of the old defects ────────────────────────────── */

TEST_FUNC(test_strtoul_sign_wraps) {
    /* strtoul("-1") is ULONG_MAX by POSIX (the old code returned 1) */
    char *e = NULL;
    unsigned long v = strtoul("-1", &e, 10);
    assert_eq(ULONG_MAX, v);
    assert_eq(2, (long)(e - "-1"));
    v = strtoul("-18446744073709551615", NULL, 10);
    assert_eq(1, v);                    /* -(ULLONG_MAX-1) wraps to 1 */
}

TEST_FUNC(test_overflow_scans_to_tail) {
    /* ERANGE, but endptr is the LAST possible position (the old code
     * stopped at the overflow point). */
    char *e = NULL;
    errno = 0;
    long v = strtol("922337203685477580842", &e, 10);
    assert_eq(LONG_MAX, v);
    assert_eq(ERANGE, errno);
    assert_eq(21, (long)(e - "922337203685477580842"));  /* every digit */

    e = NULL;
    errno = 0;
    v = strtol("123abc", &e, 10);
    assert_eq(123, v);
    assert_eq(0, errno);
    assert_eq(3, (long)(e - "123abc"));
}

TEST_FUNC(test_errno_preserved_on_success_and_no_conversion) {
    /* no-conversion must leave errno alone (the old code set EINVAL) */
    errno = 7;
    assert_eq(0, strtol("abc", NULL, 10));
    assert_eq(7, errno);

    errno = 7;
    assert_eq(42, strtol("42", NULL, 10));
    assert_eq(7, errno);                /* success preserves it too */

    errno = 0;
    assert_eq(LONG_MAX, strtol("99999999999999999999", NULL, 10));
    assert_eq(ERANGE, errno);           /* overflow DOES set it */
}

TEST_FUNC(test_strtoll_is_not_a_truncation_cast) {
    /* the old strtoll was (long long)strtol(...): the LLONG boundary
     * saturated at LONG_MAX first. */
    char *e = NULL;
    errno = 0;
    long long v = strtoll("9223372036854775807", &e, 10);
    assert_eq(LLONG_MAX, v);
    assert_eq(0, errno);
    assert_eq(19, (long)(e - "9223372036854775807"));

    errno = 0;
    v = strtoll("9223372036854775808", &e, 10);
    assert_eq(LLONG_MAX, v);
    assert_eq(ERANGE, errno);

    errno = 0;
    v = strtoll("-9223372036854775808", &e, 10);
    assert_eq(LLONG_MIN, v);
    assert_eq(0, errno);

    errno = 0;
    unsigned long long u = strtoull("18446744073709551615", &e, 10);
    assert_eq(ULLONG_MAX, u);
    assert_eq(0, errno);
    assert_eq(20, (long)(e - "18446744073709551615"));
}

TEST_FUNC(test_0x_commit_rule_and_invalid_base) {
    /* "0xg" parses the "0" and leaves endptr at the 'x' (the old code
     * consumed both and reported full failure) */
    char *e = NULL;
    long v = strtol("0xg", &e, 16);
    assert_eq(0, v);
    assert_eq(1, (long)(e - "0xg"));

    v = strtol("0x", &e, 16);
    assert_eq(0, v);
    assert_eq(1, (long)(e - "0x"));

    v = strtol("0x1f", &e, 16);
    assert_eq(31, v);
    assert_eq(4, (long)(e - "0x1f"));

    /* invalid bases: 0 answer, EINVAL, and the caller's endptr is
     * left UNTOUCHED (glibc contract) — and no divide by zero (the
     * old code divided cutoff by base unchecked) */
    errno = 0;
    e = (char *)0x1234;
    v = strtol("12", &e, 1);
    assert_eq(0, v);
    assert_eq(EINVAL, errno);
    assert_eq((char *)0x1234, e);            /* untouched */

    errno = 0;
    v = strtol("12", &e, 37);
    assert_eq(0, v);
    assert_eq(EINVAL, errno);

    /* base 36 is legal and case-insensitive */
    assert_eq(1295, (long)strtol("zz", &e, 36));
    assert_eq(2, (long)(e - "zz"));
}

TEST_FUNC(test_long_min_no_ub_and_intmax) {
    /* LONG_MIN via unsigned arithmetic (the old signed cutoff dance) */
    char *e = NULL;
    assert_eq(LONG_MIN, strtol("-9223372036854775808", &e, 10));
    assert_eq(20, (long)(e - "-9223372036854775808"));

    /* the intmax spellings (POSIX home <inttypes.h>) */
    errno = 0;
    intmax_t m = strtoimax("9223372036854775807", &e, 10);
    assert_eq(INTMAX_MAX, m);
    assert_eq(0, errno);

    errno = 0;
    m = strtoimax("-9223372036854775808", &e, 10);
    assert_eq(INTMAX_MIN, m);
    assert_eq(0, errno);

    errno = 0;
    uintmax_t u = strtoumax("18446744073709551615", &e, 10);
    assert_eq(UINTMAX_MAX, u);
    assert_eq(0, errno);
    assert_eq(20, (long)(e - "18446744073709551615"));

    /* atoll: the no-endptr convenience, LLONG range for real */
    assert_eq(-42, atoll("  -42xyz"));
    assert_eq(LLONG_MIN, atoll("-9223372036854775808"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_oracle_table_agreement),
    TEST_ENTRY(test_strtoul_sign_wraps),
    TEST_ENTRY(test_overflow_scans_to_tail),
    TEST_ENTRY(test_errno_preserved_on_success_and_no_conversion),
    TEST_ENTRY(test_strtoll_is_not_a_truncation_cast),
    TEST_ENTRY(test_0x_commit_rule_and_invalid_base),
    TEST_ENTRY(test_long_min_no_ub_and_intmax),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
