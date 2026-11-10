/* hosttests/data/floatscan_cases.h — the deterministic float-parse
 * table shared by the T11 test (test_libc_float_parse.c, linked
 * against the OS01 libc) and the standalone glibc oracle
 * (hosttests/oracle/floatscan_oracle.c, compiled with NO OS01
 * includes).  Both sides run strtod/strtof/strtold over every row and
 * compare BIT PATTERNS (the raw IEEE bytes, padding bytes masked off
 * for the 80-bit long double) plus the consumed length and errno.
 *
 * Both implementations are correctly rounded (glibc by contract,
 * musl's floatscan by algorithm), so agreement is bit-exact for every
 * row whose semantics are fixed by IEEE-754 + POSIX — which is all of
 * these.
 */
#ifndef OS01_FLOATSCAN_CASES_H
#define OS01_FLOATSCAN_CASES_H

static const struct { const char *input; } float_cases[] = {
    /* exact decimal / rounding-critical values */
    "0.5",
    "2.5",
    "-2.5",
    "1.00000000000000001",
    "0.1",
    "0.3",
    "3.141592653589793238462643383279",
    "2.718281828459045235360287471352",

    /* long-digit inputs (musl's bignum path vs glibc's GMP-free path) */
    "123456789012345678901234567890.1234567890123456789",
    "3.14159265358979323846264338327950288419716939937510582097494459",

    /* exponents */
    "1e0",
    "1e1",
    "1e10",
    "1e308",
    "1e-308",
    "-1e308",
    "1.7976931348623157e308",               /* DBL_MAX */
    "1.7976931348623158e308",               /* DBL_MAX + rounding up → inf? */
    "2.2250738585072014e-308",              /* DBL_MIN */
    "4.9406564584124654e-324",              /* smallest subnormal */
    "1e-320",                               /* mid-subnormal */
    "1e400",                                /* overflow: inf + ERANGE */
    "-1e400",
    "1e-400",                               /* underflow: 0 + ERANGE */

    /* hex floats */
    "0x1.8p+1",                             /* 3.0 */
    "0x1p-1",
    "0x1.fffffffffffffp+1023",              /* just under DBL_MAX */
    "0x1p-1074",                            /* smallest subnormal */
    "-0x1.8p+1",

    /* INF / NAN / negative zero */
    "inf",
    "infinity",
    "INF",
    "Infinity",
    "-inf",
    "-INFINITY",
    "nan",
    "-nan",
    "NAN",
    "nan(123)",
    "-0.0",
    "-0",

    /* partial-consumption and no-consumption shapes */
    "1e",                                   /* consumes '1', endptr at 'e' */
    "1e+",                                  /* consumes '1' */
    "1e-",                                  /* consumes '1' */
    "42abc",
    "  12.5xyz",
    "",
    "   ",
    "abc",
    ".e1",
    "+",
    "-",
    "0x",
    "0xg",
    ".",                                    /* POSIX: '.' alone → 0 consumed */
};

#define FLOAT_CASE_COUNT (int)(sizeof(float_cases) / sizeof(float_cases[0]))

#endif /* OS01_FLOATSCAN_CASES_H */
