// libc/stdlib/strtod.c — the strtod family on the musl floatscan core
// (T11).
//
// The previous implementation multiplied/divided its way to the result
// digit by digit: long inputs lost low-order bits, hex floats and
// INF/NAN were unparsed, ERANGE never raised, "1e+" consumed the 'e',
// and long double could not exist at all.  All four spellings now run
// the precision-aware scanner (__floatscan, prec 0/1/2) so a long
// double result never rounds through a double.
//
// errno discipline (glibc/POSIX practice, and our T10 strtox
// convention — a deliberate divergence from musl upstream, recorded in
// thirdpart/musl-libc-algorithms.manifest): ERANGE on genuine
// overflow/underflow; errno PRESERVED when nothing was consumed (the
// scanner sets EINVAL there; the wrappers undo it).  endptr: after the
// consumed text; nptr itself when nothing was consumed.

#include <errno.h>
#include <float.h>
#include <stdlib.h>
#include <stdlib/floatscan.h>
static int scan_is_hex(const char *s);

/* the target type's smallest NORMAL for the requested precision */
static fs_float_t fs_type_min(int prec)
{
    switch (prec) {
    case 0:  return FLT_MIN;
    case 1:  return DBL_MIN;
    default: return FS_LDBL_MIN;
    }
}

/* Shared body — musl's src/stdlib/strtod.c strtox() shape, plus two
 * deliberate divergences recorded in the manifest:
 *   - the scanner's EINVAL marks a NO-CONVERSION scan: endptr = nptr
 *     and errno restored (glibc practice; musl leaves endptr past the
 *     whitespace and sets EINVAL);
 *   - glibc raises ERANGE for EVERY inexact subnormal result, musl
 *     only for some — the wrappers re-raise it by checking the result
 *     against the target type's MIN (POSIX: "may fail"; the oracle
 *     contract wants glibc's answer). */
static fs_float_t strtod_scan(const char *s, char **endptr, int prec)
{
    int saved_errno = errno;

    shfile_t f;
    sh_fromstring(&f, s);
    shlim(&f, 0);
    fs_float_t y = __floatscan(&f, prec, 1);
    long cnt = shcnt(&f);

    if (errno == EINVAL && saved_errno != EINVAL) {
        /* no conversion: endptr = nptr, errno untouched */
        errno = saved_errno;
        if (endptr) *endptr = (char *)s;
        return (fs_float_t)0;
    }

    if (endptr) *endptr = cnt ? (char *)s + cnt : (char *)s;
    if (cnt == 0)
        errno = saved_errno;

    /* inexact subnormal ⇒ ERANGE (glibc practice).  A HEX input can
     * be EXACTLY subnormal ("0x1p-1074"), and glibc answers no ERANGE
     * for those — the check therefore applies to decimal inputs only,
     * whose subnormal results are practically always inexact. */
    if (!scan_is_hex(s) && y != (fs_float_t)0 &&
        fs_fabs(y) < fs_type_min(prec))
        errno = ERANGE;
    return y;
}

/* was the scanned value a hex float ("0x…" after the sign)? */
static int scan_is_hex(const char *s)
{
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (*s == '+' || *s == '-') s++;
    return s[0] == '0' && (s[1] | 32) == 'x';
}

double strtod(const char *nptr, char **endptr)
{
    fs_float_t y = strtod_scan(nptr, endptr, 1);
    return (y != y) ? fs_narrow_nand(y) : fs_to_double(y);
}

float strtof(const char *nptr, char **endptr)
{
    fs_float_t y = strtod_scan(nptr, endptr, 0);
    return (y != y) ? fs_narrow_nanf(y) : fs_to_float(y);
}

long double strtold(const char *nptr, char **endptr)
{
    return fs_to_ldbl(strtod_scan(nptr, endptr, 2));
}

double atof(const char *nptr)
{
    return strtod(nptr, NULL);
}
