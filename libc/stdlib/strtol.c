// libc/stdlib/strtol.c — the shared integer-conversion core (T10) and
// the long/unsigned-long spellings.
//
// What the previous implementation got wrong, and what the core now
// does instead (each item is pinned by test_libc_integer.c):
//   - strtoul("-1") returned 1: the sign was parsed but never applied.
//     Unsigned wrapping is the POSIX answer: strtoul("-1") == ULONG_MAX.
//   - LONG_MIN saturated via -(LONG_MAX)-1 arithmetic on a signed long
//     (cutoff computed from a negative value, then negated again) —
//     fragile at best, UB at worst.  The core accumulates in uintmax_t
//     against an unsigned cap chosen by the sign; LONG_MIN's magnitude
//     (2^63) is just a number there.
//   - An overflowing run STOPPED scanning at the overflow point, so
//     endptr landed mid-digits.  POSIX wants the LAST possible
//     position even on ERANGE; the core keeps scanning to the tail.
//   - errno=EINVAL on no-conversion: POSIX says errno is untouched
//     there (EINVAL only for an invalid base).  The wrappers preserve
//     errno on success and on no-conversion alike.
//   - "0x" with no following hex digit consumed both characters and
//     reported a full failure.  POSIX parses the "0" and leaves endptr
//     after it; the core only commits the 0x when a hex digit follows.
//   - base 1 / 37 walked straight into `cutoff /= base` nonsense.
//     The wrappers validate 2..36-or-0 up front: 0 return, endptr =
//     nptr, errno = EINVAL (glibc's answer, and the only one that
//     cannot divide by zero).

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdlib/strtox.h>

struct __strtox_result __strtox(const char *nptr, int base,
                                uintmax_t max_pos, uintmax_t max_neg)
{
    struct __strtox_result r = { 0, nptr, 0, 0, 0 };
    const char *s = nptr;

    while (isspace((unsigned char)*s)) s++;

    if (*s == '-') { r.neg = 1; s++; }
    else if (*s == '+') s++;

    /* Base detection with the POSIX "0x" commit rule: the leading 0
     * counts as a digit of an octal/auto parse even when the "0x" is
     * incomplete ("0xg" parses as "0" ending at 'x'). */
    if ((base == 0 || base == 16) && *s == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        int d = (s[2] >= '0' && s[2] <= '9') ? s[2] - '0'
              : (s[2] >= 'a' && s[2] <= 'f') ? s[2] - 'a' + 10
              : (s[2] >= 'A' && s[2] <= 'F') ? s[2] - 'A' + 10
              : -1;
        if (d >= 0 && d < 16) {
            s += 2;
            base = 16;
        } else if (base == 0) {
            base = 8;
        } else {
            base = 16;      /* explicit 16: the bare "0" is a digit */
        }
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    uintmax_t cap = r.neg ? max_neg : max_pos;
    uintmax_t cutoff = cap / (uintmax_t)base;
    uintmax_t cutlim = cap % (uintmax_t)base;

    while (*s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;

        r.any = 1;
        if (r.val > cutoff || (r.val == cutoff && (uintmax_t)d > cutlim)) {
            r.range = 1;
            r.val = cap;            /* saturated; keep scanning */
        } else {
            r.val = r.val * (uintmax_t)base + (uintmax_t)d;
        }
        s++;
    }

    r.end = s;
    return r;
}

/* The wrapper boilerplate: validate the base, run the core, publish
 * endptr, and answer in the spelling's own type.  errno: ERANGE only
 * on overflow (the saturated value is returned); success and
 * no-conversion leave errno untouched.  A no-conversion run answers 0
 * with endptr = nptr. */
static long strtol_wrap(const char *nptr, char **endptr, int base)
{
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        /* glibc leaves the caller's endptr UNTOUCHED on an invalid
         * base; so do we (only the oracle-visible contract is
         * affected — no-conversion still answers endptr = nptr). */
        return 0;
    }

    struct __strtox_result r = __strtox(nptr, base,
                                        LONG_MAX,
                                        (uintmax_t)LONG_MAX + 1);
    if (endptr) *endptr = (char *)(r.any ? r.end : nptr);
    if (r.range) errno = ERANGE;
    if (!r.any) return 0;
    /* unsigned negation + cast: no signed overflow anywhere */
    return r.neg ? (long)(-r.val) : (long)r.val;
}

static unsigned long strtoul_wrap(const char *nptr, char **endptr, int base)
{
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;
    }

    struct __strtox_result r = __strtox(nptr, base,
                                        ULONG_MAX, ULONG_MAX);
    if (endptr) *endptr = (char *)(r.any ? r.end : nptr);
    if (r.range) {
        errno = ERANGE;
        /* POSIX: an unsigned overflow answers the type's MAX — the
         * sign is NOT applied after a clamp (strtoull("-1") wraps, but
         * strtoull("-<overflow>") is MAX, per glibc). */
        return ULONG_MAX;
    }
    if (!r.any) return 0;
    return r.neg ? (unsigned long)(-r.val) : (unsigned long)r.val;
}

long strtol(const char *nptr, char **endptr, int base)
{
    return strtol_wrap(nptr, endptr, base);
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    return strtoul_wrap(nptr, endptr, base);
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}
