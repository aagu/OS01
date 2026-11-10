/* hosttests/data/strtox_cases.h — the deterministic-semantics table
 * shared by the T10 integer-conversion test (test_libc_integer.c,
 * linked against the OS01 libc) and the standalone glibc oracle
 * (hosttests/oracle/strtox_oracle.c, compiled with NO OS01 includes so
 * it cannot help but use the host's strto* family).  Both processes
 * evaluate every row and the test compares the full result tuples.
 *
 * Row = { base, input }.  Only rows whose semantics every strto*
 * implementation must agree on (POSIX + glibc practice): boundary
 * values, the ±1-digit overflows, unsigned sign wrapping, overflow
 * continuation to the tail, 0x commit rules, invalid bases, empty and
 * whitespace-only inputs, errno preservation (both sides zero errno
 * before each call and report the post-call value).
 */
#ifndef OS01_STRTOX_CASES_H
#define OS01_STRTOX_CASES_H

static const struct { int base; const char *input; } strtox_cases[] = {
    /* exact boundary values, decimal */
    { 10, "9223372036854775807" },            /* LLONG_MAX */
    { 10, "9223372036854775808" },            /* LLONG_MAX + 1 */
    { 10, "-9223372036854775808" },           /* LLONG_MIN */
    { 10, "-9223372036854775809" },           /* LLONG_MIN - 1 */
    { 10, "2147483647" },                     /* INT_MAX */
    { 10, "-2147483648" },                    /* INT_MIN */
    { 10, "18446744073709551615" },           /* UINTMAX_MAX */
    { 10, "18446744073709551616" },           /* UINTMAX_MAX + 1 */
    { 10, "-18446744073709551615" },          /* -(UINTMAX_MAX-1) */

    /* boundary values one digit past, other bases */
    { 16, "7fffffffffffffff" },               /* LLONG_MAX */
    { 16, "8000000000000000" },               /* LLONG_MAX + 1 */
    { 16, "-8000000000000000" },              /* LLONG_MIN */
    { 8,  "1777777777777777777777" },         /* LLONG_MAX octal */
    { 8,  "2000000000000000000000" },         /* LLONG_MAX + 1 octal */
    { 36, "1z141z380g" },                     /* LLONG_MAX base 36-ish */
    { 2,  "111111111111111111111111111111111111111111111111111111111111111" },
    { 2,  "-1000000000000000000000000000000000000000000000000000000000000000" },

    /* unsigned sign wrapping (POSIX: strtoull("-1") == ULLONG_MAX) */
    { 10, "-1" },
    { 10, "-2" },
    { 16, "-1" },
    { 10, "-18446744073709551615" },

    /* overflow keeps scanning to the LAST possible position */
    { 10, "99999999999999999999999999" },
    { 10, "123456789012345678901234567890abc" },
    { 16, "ffffffffffffffffff" },
    { 10, "-99999999999999999999999999xyz" },

    /* 0x commit rules */
    { 16, "0x1f" },
    { 0,  "0x1f" },
    { 0,  "0xg" },                            /* "0" then 'x' unconsumed */
    { 16, "0x" },                             /* bare 0x: parses "0" */
    { 0,  "010" },                            /* octal */
    { 10, "010" },
    { 0,  "0" },
    { 8,  "0x11" },                           /* base 8: 0 then 'x' stops */

    /* base 36 mixed case */
    { 36, "zZ" },
    { 36, "1z141z4" },

    /* whitespace, signs, no-conversion */
    { 10, "" },
    { 10, "   " },
    { 10, "+" },
    { 10, "-" },
    { 10, "+-3" },
    { 10, "  +42" },
    { 10, "  -42junk" },
    { 10, "abc" },

    /* invalid bases: 0 answer, endptr = nptr, EINVAL */
    { 1,  "12" },
    { 37, "12" },
    { -1, "12" },
};

#define STRTOX_CASE_COUNT (int)(sizeof(strtox_cases) / sizeof(strtox_cases[0]))

#endif /* OS01_STRTOX_CASES_H */
