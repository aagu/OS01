/*
 * test_libc_scan.c — libc-completion Task T12: the in-memory scanf core.
 *
 * Layer 1 — THE ORACLE TABLE: the shared table
 * (hosttests/data/scan_cases.h) is evaluated by the standalone glibc
 * oracle (hosttests/oracle/scanf_oracle.c, a SEPARATE process compiled
 * with no OS01 include paths) and by THIS binary against the OS01
 * vsscanf/sscanf.  Both sides call the scan with four pointer arguments
 * and compare the raw bytes of every argument slot: the conversion
 * decides how many bytes are written, so a conversion that stores the
 * wrong type (the old sscanf.c wrote an int for %hhd), the wrong number
 * of bytes (a %3c that wrongly NUL-terminates) or into the wrong
 * argument (a suppressed conversion that wrongly consumed a va_arg)
 * all show up as a byte difference.
 *
 * Layer 2 — THE DIRECT ASSERTIONS: the properties the oracle cannot
 * pin (the deliberate divergences from glibc are asserted here, with
 * the reason): the width semantics, the typed stores, %n, the
 * suppression, the scan-set rules, the explicit termination on an
 * unsupported format and the guard-page check of the /proc maps line.
 *
 * NOTE: assert_eq() stringizes its expression into a printf format, so
 * every call whose format string contains a '%' is hoisted into a
 * variable first (a '%' inside the stringized expression would crash
 * the framework's failure-message formatter).
 */

#include "test_framework.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../data/scan_cases.h"

/* baked in by the make rule; stays inside the profile build dir */
#ifndef SCANF_ORACLE_PATH
#define SCANF_ORACLE_PATH "scanf_oracle-missing"
#endif

/* The RED build (the stub that shipped before T12) has no vsscanf at
 * all; the vsscanf-only assertions are compiled out there so that the
 * RED run is a BEHAVIOURAL failure and not a link error. */
#ifdef SCAN_RED_STUB
#define HAVE_VSSCANF 0
#else
#define HAVE_VSSCANF 1
#endif

/* a slot is 8-byte aligned so that a double / long double store is
 * aligned the way the callee expects */
union scan_slot {
    unsigned char b[32];
    long         l;
    double       d;
    long double  ld;
};

#define POISON 0xA5

static void poison_slots(union scan_slot slot[4])
{
    memset(slot, POISON, sizeof(union scan_slot) * 4);
}

static void dump_slot(char *out, const union scan_slot *slot, int kind)
{
    for (int i = 0; i < SCAN_SLOT_BYTES; i++) {
        if (kind == SK_LDBL && i >= SCAN_LDBL_BYTES) {
            out[2 * i]     = 'x';
            out[2 * i + 1] = 'x';
            continue;
        }
        sprintf(out + 2 * i, "%02x", slot->b[i]);
    }
    out[2 * SCAN_SLOT_BYTES] = '\0';
}

/* build the canonical row of one case: id, count and the four slots */
static void build_case_row(char *out, size_t cap, int n,
                           union scan_slot slot[4], const unsigned char kinds[4],
                           const char *id)
{
    char hex[4][2 * SCAN_SLOT_BYTES + 1];

    for (int k = 0; k < 4; k++)
        dump_slot(hex[k], &slot[k], kinds[k]);
    snprintf(out, cap, "%s\t%d\t%s\t%s\t%s\t%s\n",
             id, n, hex[0], hex[1], hex[2], hex[3]);
}

/* the vsscanf entry point, with the four pointer arguments */
static int scan_v(const char *in, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsscanf(in, fmt, ap);
    va_end(ap);
    return n;
}

TEST_FUNC(test_oracle_byte_agreement) {
    static char out[16 * 1024];

    int fds[2];
    assert_eq(0, pipe(fds));
    int64_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *argv[2] = { (char *)SCANF_ORACLE_PATH, NULL };
        execv(SCANF_ORACLE_PATH, argv);
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

    int mismatches = 0;
    const char *cursor = out;
    for (int i = 0; i < SCAN_CASE_COUNT; i++) {
        char want[512], got[512];
        union scan_slot slot[4];

        const char *nl = strchr(cursor, '\n');
        if (!nl) {
            printf("  [FAIL] oracle produced only %d of %d rows\n",
                   i, SCAN_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor) + 1;
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;

        /* the OS01 side, through sscanf */
        poison_slots(slot);
        int n = sscanf(scan_cases[i].input, scan_cases[i].fmt,
                       &slot[0], &slot[1], &slot[2], &slot[3]);
        build_case_row(got, sizeof(got), n, slot, scan_cases[i].kinds,
                       scan_cases[i].id);

        if (strcmp(want, got) != 0) {
            printf("  [FAIL] case %d (id=%s)\n"
                   "         oracle: %s"
                   "         os01:   %s",
                   i, scan_cases[i].id, want, got);
            mismatches++;
        }

#if HAVE_VSSCANF
        /* and through vsscanf, which must answer identically */
        {
            char got_v[512];
            poison_slots(slot);
            int nv = scan_v(scan_cases[i].input, scan_cases[i].fmt,
                            &slot[0], &slot[1], &slot[2], &slot[3]);
            build_case_row(got_v, sizeof(got_v), nv, slot, scan_cases[i].kinds,
                           scan_cases[i].id);
            if (strcmp(want, got_v) != 0) {
                printf("  [FAIL] vsscanf case %d (id=%s)\n"
                       "         oracle: %s"
                       "         os01:   %s",
                       i, scan_cases[i].id, want, got_v);
                mismatches++;
            }
        }
#endif
    }
    if (*cursor != '\0') {
        printf("  [FAIL] oracle produced MORE rows than the table\n");
        mismatches++;
    }
    assert_eq(0, mismatches);
}

TEST_FUNC(test_width_limits_the_scanned_input) {
    int a = 0, b = 0;
    int n;

    /* the width bounds the digits the conversion may consume */
    n = sscanf("12345", "%3d", &a);
    assert_eq(1, n);
    assert_eq(123, a);

    /* the rest of the field is still there for the next conversion */
    n = sscanf("12345", "%2d%d", &a, &b);
    assert_eq(2, n);
    assert_eq(12, a);
    assert_eq(345, b);

    /* the whitespace in front of the number is NOT counted against the
     * width (glibc: "   12345" with "%5d" is 12345) */
    n = sscanf("   12345", "%5d", &a);
    assert_eq(1, n);
    assert_eq(12345, a);

    /* a width of 0 means "no limit" */
    n = sscanf("12345", "%0d", &a);
    assert_eq(1, n);
    assert_eq(12345, a);

    /* the width also bounds a float field: "%5f" sees "3.141" */
    {
        float f = 0;
        n = sscanf("3.14159265", "%5f", &f);
        assert_eq(1, n);
        assert_true(f > 3.1409f && f < 3.1411f);
    }

    /* a width on %s is a MAXIMUM, and the NUL is written after it */
    {
        char buf[16];
        memset(buf, POISON, sizeof(buf));
        n = sscanf("hello world", "%5s", buf);
        assert_eq(1, n);
        assert_str_eq("hello", buf);
        assert_eq(0, buf[5]);
    }
    {
        char buf[16];
        memset(buf, POISON, sizeof(buf));
        n = sscanf("ab cd", "%5s", buf);
        assert_eq(1, n);
        assert_str_eq("ab", buf);
        assert_eq(0, buf[2]);
    }
}

TEST_FUNC(test_length_modifier_stores) {
    signed char sc = 0;
    short sh = 0;
    int i = 0;
    long l = 0;
    long long ll = 0;
    intmax_t im = 0;
    size_t z = 0;
    ptrdiff_t pd = 0;
    unsigned long long ull = 0;
    long double ld = 0;
    int n;

    n = sscanf("127", "%hhd", &sc);
    assert_eq(1, n);
    assert_eq(127, (int)sc);

    n = sscanf("32767", "%hd", &sh);
    assert_eq(1, n);
    assert_eq(32767, (int)sh);

    n = sscanf("2147483648", "%d", &i);
    assert_eq(1, n);
    assert_eq(-2147483648, i);

    n = sscanf("4294967295", "%ld", &l);
    assert_eq(1, n);
    assert_eq(4294967295L, l);

    n = sscanf("-9223372036854775807", "%lld", &ll);
    assert_eq(1, n);
    assert_eq(-9223372036854775807LL, ll);

    /* %jd really is an intmax_t */
    n = sscanf("1234567890123", "%jd", &im);
    assert_eq(1, n);
    assert_eq(1234567890123LL, (long long)im);

    /* %zu really is a size_t (unsigned) */
    n = sscanf("18446744073709551615", "%zu", &z);
    assert_eq(1, n);
    assert_true(z == 18446744073709551615ULL);

    /* %td really is a ptrdiff_t (signed) */
    n = sscanf("-100", "%td", &pd);
    assert_eq(1, n);
    assert_eq(-100, (int)pd);

    /* %Lf really is a long double */
    n = sscanf("3.14159265358979323846", "%Lf", &ld);
    assert_eq(1, n);
    assert_true(ld > 3.141592653589793238L);
    assert_true(ld < 3.141592653589793239L);

    /* the unsigned spellings keep the full 64-bit value */
    n = sscanf("18446744073709551615", "%llu", &ull);
    assert_eq(1, n);
    assert_true(ull == 18446744073709551615ULL);
}

TEST_FUNC(test_out_of_range_integer_stores) {
    long long ll = 0;
    int i = 0;
    int n;

    /* A magnitude that does NOT fit in 64 bits is out of range for every
     * target type: OS01 clamps it to the target's range (the maximum
     * for a positive value, the minimum for a negative one).  glibc
     * gives the same answer for its 8-byte signed targets (LONG_MIN),
     * and it agrees with this libc's own strto* convention (T10
     * saturates at LONG_MAX/LONG_MIN). */
    n = sscanf("-99999999999999999999", "%lld", &ll);
    assert_eq(1, n);
    assert_eq(-9223372036854775807LL - 1LL, ll);

    ll = 0;
    n = sscanf("99999999999999999999", "%lld", &ll);
    assert_eq(1, n);
    assert_eq(9223372036854775807LL, ll);

    /* the same rule for the 4-byte target */
    i = 0;
    n = sscanf("-99999999999999999999", "%d", &i);
    assert_eq(1, n);
    assert_eq(-2147483647 - 1, i);

    /* a magnitude that FITS in 64 bits but not in the target type is
     * TRUNCATED to the two's-complement low bits (glibc's answer for
     * the 1/2/4-byte targets: "%hhd" of "300" is 44) */
    {
        signed char c = 0;
        n = sscanf("300", "%hhd", &c);
        assert_eq(1, n);
        assert_eq(44, (int)c);
    }
    {
        short sh = 0;
        n = sscanf("70000", "%hd", &sh);
        assert_eq(1, n);
        assert_eq(4464, (int)sh);
    }
    {
        unsigned u = 0;
        n = sscanf("-1", "%u", &u);
        assert_eq(1, n);
        assert_eq(4294967295u, u);
    }
}

TEST_FUNC(test_n_stores_the_consumed_length) {
    int v = 0;
    int n = -1;
    int r;

    /* %n stores the number of characters consumed so far ... */
    r = sscanf("123", "%d%n", &v, &n);
    assert_eq(1, r);
    assert_eq(123, v);
    assert_eq(3, n);

    /* ... and it does NOT count as a conversion */
    r = sscanf("123", "%d%n", &v, &n);
    assert_eq(1, r);
    r = sscanf("abc", "%n", &n);
    assert_eq(0, r);
    assert_eq(0, n);

    /* the whitespace the format skipped counts as consumed */
    r = sscanf("  abc", " %n", &n);
    assert_eq(0, r);
    assert_eq(2, n);
}

TEST_FUNC(test_suppressed_conversion_takes_no_argument) {
    int v = 0;
    int n;

    /* the suppressed %d consumes NO argument: the %d behind it must
     * read the SECOND number */
    n = sscanf("5 7", "%*d %d", &v);
    assert_eq(1, n);
    assert_eq(7, v);

    /* a suppressed conversion that does not match does not count */
    v = 111;
    n = sscanf("x", "%*d", &v);
    assert_eq(0, n);
    assert_eq(111, v);
}

TEST_FUNC(test_input_failure_and_matching_failure) {
    int v = 111;
    int n;

    /* an empty input is an input failure: EOF */
    n = sscanf("", "%d", &v);
    assert_eq(-1, n);
    assert_eq(111, v);

    /* a whitespace-only input is an input failure as well */
    n = sscanf("   ", "%d", &v);
    assert_eq(-1, n);

    /* a first character that does not match is a MATCHING failure: 0 */
    n = sscanf("abc", "%d", &v);
    assert_eq(0, n);
    assert_eq(111, v);

    /* after a completed conversion the count is kept */
    n = sscanf("1", "%d%d", &v, &v);
    assert_eq(1, n);
    assert_eq(1, v);
}

TEST_FUNC(test_char_and_string_rules) {
    char buf[16];
    int n;

    /* %c does not skip whitespace */
    memset(buf, POISON, sizeof(buf));
    n = sscanf(" abc", "%c", buf);
    assert_eq(1, n);
    assert_eq(' ', buf[0]);
    assert_eq(POISON, buf[1] & 0xff);

    /* %3c reads three characters and does NOT NUL-terminate */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("abc", "%3c", buf);
    assert_eq(1, n);
    assert_eq('a', buf[0]);
    assert_eq('b', buf[1]);
    assert_eq('c', buf[2]);
    assert_eq(POISON, buf[3] & 0xff);

    /* %s stops at the whitespace and leaves the NUL behind */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("hello world", "%s", buf);
    assert_eq(1, n);
    assert_str_eq("hello", buf);
    assert_eq(0, buf[5]);

    /* the NUL lands right after the token */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("ab cd", "%s", buf);
    assert_eq(1, n);
    assert_str_eq("ab", buf);
    assert_eq(0, buf[2]);
    assert_eq(POISON, buf[3] & 0xff);
}

TEST_FUNC(test_scanset_rules) {
    char buf[16];
    int n;

    /* the negated set stops at the first excluded character */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("a1b2", "%[^0-9]", buf);
    assert_eq(1, n);
    assert_str_eq("a", buf);
    assert_eq(0, buf[1]);

    /* a leading ']' is an ordinary character of the set */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("]abc", "%[]]", buf);
    assert_eq(1, n);
    assert_str_eq("]", buf);

    /* a range */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("abcde", "%[a-c]", buf);
    assert_eq(1, n);
    assert_str_eq("abc", buf);

    /* a set that does not match at all leaves the buffer alone */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("xyz", "%[a-c]", buf);
    assert_eq(0, n);
    assert_eq(POISON, buf[0] & 0xff);

    /* the POSIX character classes (a POSIX feature glibc's scanf does
     * not implement — see the T12 report) */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("abc1", "%[[:alpha:]]", buf);
    assert_eq(1, n);
    assert_str_eq("abc", buf);

    memset(buf, POISON, sizeof(buf));
    n = sscanf("123abc", "%[[:digit:]]", buf);
    assert_eq(1, n);
    assert_str_eq("123", buf);

    /* %[ does NOT skip leading whitespace: the '[' input item is "a
     * sequence of characters of exactly those in the set", so a leading
     * space that is not in the set is a MATCHING FAILURE — the count
     * stays 0 and the destination is left untouched (POSIX and glibc). */
    memset(buf, POISON, sizeof(buf));
    n = sscanf(" abc", "%[abc]", buf);
    assert_eq(0, n);
    assert_eq(POISON, buf[0] & 0xff);

    /* a whitespace-only input is a matching failure as well (NOT EOF) */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("  ", "%[a-c]", buf);
    assert_eq(0, n);
    assert_eq(POISON, buf[0] & 0xff);

    /* a tab in front of the set is no different */
    memset(buf, POISON, sizeof(buf));
    n = sscanf(" \tabc", "%[abc]", buf);
    assert_eq(0, n);
    assert_eq(POISON, buf[0] & 0xff);

    /* the negated set does not skip the whitespace either */
    memset(buf, POISON, sizeof(buf));
    n = sscanf(" abc", "%[^ ]", buf);
    assert_eq(0, n);
    assert_eq(POISON, buf[0] & 0xff);

    /* a space that IS a member of the set is matched and stored */
    memset(buf, POISON, sizeof(buf));
    n = sscanf(" abc", "%[ abc]", buf);
    assert_eq(1, n);
    assert_str_eq(" abc", buf);

    /* an empty input is an input failure (EOF), as for every other
     * conversion that consumes input */
    memset(buf, POISON, sizeof(buf));
    n = sscanf("", "%[a-c]", buf);
    assert_eq(-1, n);
    assert_eq(POISON, buf[0] & 0xff);
}

TEST_FUNC(test_literal_percent_and_pointer) {
    int v = 0;
    void *p = NULL;
    int n;

    /* a literal %% */
    n = sscanf("100%", "%d%%", &v);
    assert_eq(1, n);
    assert_eq(100, v);

    /* %p parses a hexadecimal pointer value */
    n = sscanf("0x7f001234", "%p", &p);
    assert_eq(1, n);
    assert_true(p == (void *)0x7f001234UL);

    /* trailing whitespace in the format matches any amount of input
     * whitespace, including none */
    n = sscanf("1   ", "%d   ", &v);
    assert_eq(1, n);
    assert_eq(1, v);
    n = sscanf("1", "%d   ", &v);
    assert_eq(1, n);
    assert_eq(1, v);
}

TEST_FUNC(test_unsupported_format_terminates_explicitly) {
    int a = 0, b = 0;
    int n;

    /* an unknown conversion stops the scan: the conversion behind it is
     * NOT performed and no further argument is consumed */
    a = 0; b = 0;
    n = sscanf("1 2", "%d %q %d", &a, &b);
    assert_eq(1, n);
    assert_eq(1, a);
    assert_eq(0, b);

    /* an ILLEGAL length-modifier combination is rejected the same way
     * (glibc silently ignores the modifier; the task requires the
     * rejection) */
    a = 0; b = 0;
    n = sscanf("1 2", "%d %Ld %d", &a, &b);
    assert_eq(1, n);
    assert_eq(1, a);
    assert_eq(0, b);

    a = 0; b = 0;
    n = sscanf("1 2", "%d %ln %d", &a, &b);
    assert_eq(1, n);
    assert_eq(1, a);
    assert_eq(0, b);

    a = 0; b = 0;
    n = sscanf("1 2", "%d %hs %d", &a, &b);
    assert_eq(1, n);
    assert_eq(1, a);
    assert_eq(0, b);

    /* an unterminated scan set is rejected as well */
    a = 0; b = 0;
    n = sscanf("1 2", "%d %[abc %d", &a, &b);
    assert_eq(1, n);
    assert_eq(1, a);
    assert_eq(0, b);
}

TEST_FUNC(test_proc_maps_style_line) {
    /* the /proc/<pid>/maps line the kernel test parses:
     *   "<start>-<end> <perms> ..." — the addresses are 64 bit and the
     * permission field is four characters wide */
    unsigned long start = 0, end = 0;
    char perm[8];
    const char *line = "7f1234567890-7f1234567abc r--p 00000000 00:00 0 \n";
    int fields;

    memset(perm, POISON, sizeof(perm));
    fields = sscanf(line, "%lx-%lx %4s", &start, &end, perm);
    assert_eq(3, fields);
    assert_true(start == 0x7f1234567890UL);
    assert_true(end == 0x7f1234567abcUL);
    assert_str_eq("r--p", perm);

    /* the legacy guard page must not fall into a parsed range */
    {
        const char *guarded = "20400000-20600000 rw-p 00000000 00:00 0 \n";
        unsigned long s2 = 0, e2 = 0;
        fields = sscanf(guarded, "%lx-%lx %4s", &s2, &e2, perm);
        assert_eq(3, fields);
        assert_true(s2 == 0x20400000UL);
        assert_true(e2 == 0x20600000UL);
        /* the guard page 0x600000 is OUTSIDE that range */
        assert_false(s2 <= 0x600000UL && 0x600000UL < e2);
    }
}

#if HAVE_VSSCANF
TEST_FUNC(test_vsscanf_matches_sscanf) {
    int a = 0, b = 0;
    int va = 0, vb = 0;
    char buf1[16], buf2[16];
    int n1, n2;

    memset(buf1, POISON, sizeof(buf1));
    memset(buf2, POISON, sizeof(buf2));

    n1 = sscanf("42 7", "%d %d", &a, &b);
    n2 = scan_v("42 7", "%d %d", &va, &vb);
    assert_eq(n1, n2);
    assert_eq(a, va);
    assert_eq(b, vb);

    n1 = sscanf("hello", "%s", buf1);
    n2 = scan_v("hello", "%s", buf2);
    assert_eq(n1, n2);
    assert_str_eq(buf1, buf2);

    /* the EOF answer is the same through both entry points */
    n2 = scan_v("", "%d", &va);
    assert_eq(-1, n2);
    n2 = scan_v("abc", "%d", &va);
    assert_eq(0, n2);
}
#endif

TEST_LIST_BEGIN
    TEST_ENTRY(test_oracle_byte_agreement),
    TEST_ENTRY(test_width_limits_the_scanned_input),
    TEST_ENTRY(test_length_modifier_stores),
    TEST_ENTRY(test_out_of_range_integer_stores),
    TEST_ENTRY(test_n_stores_the_consumed_length),
    TEST_ENTRY(test_suppressed_conversion_takes_no_argument),
    TEST_ENTRY(test_input_failure_and_matching_failure),
    TEST_ENTRY(test_char_and_string_rules),
    TEST_ENTRY(test_scanset_rules),
    TEST_ENTRY(test_literal_percent_and_pointer),
    TEST_ENTRY(test_unsupported_format_terminates_explicitly),
    TEST_ENTRY(test_proc_maps_style_line),
#if HAVE_VSSCANF
    TEST_ENTRY(test_vsscanf_matches_sscanf),
#endif
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
