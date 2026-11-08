// libc/stdlib/strtoll.c — the long long spellings (T10).
//
// The previous versions were truncation casts of strtol/strtoul, so
// anything past the LONG range saturated at LONG_MAX and then got
// cast — strtoll("9223372036854775808") answered LONG_MAX instead of
// LLONG_MAX+ERANGE.  These call the shared __strtox core with the
// long long caps directly; strtoimax/strtoumax (POSIX home
// <inttypes.h>) are the intmax_t spellings of the same two calls,
// and atoll() is the no-endptr convenience.

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdlib/strtox.h>

long long strtoll(const char *nptr, char **endptr, int base)
{
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;                       /* endptr left untouched, like glibc */
    }

    struct __strtox_result r = __strtox(nptr, base,
                                        LLONG_MAX,
                                        (uintmax_t)LLONG_MAX + 1);
    if (endptr) *endptr = (char *)(r.any ? r.end : nptr);
    if (r.range) errno = ERANGE;
    if (!r.any) return 0;
    return r.neg ? (long long)(-r.val) : (long long)r.val;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;
    }

    struct __strtox_result r = __strtox(nptr, base,
                                        ULLONG_MAX, ULLONG_MAX);
    if (endptr) *endptr = (char *)(r.any ? r.end : nptr);
    if (r.range) {
        errno = ERANGE;
        return ULLONG_MAX;              /* sign not applied after a clamp */
    }
    if (!r.any) return 0;
    return r.neg ? (unsigned long long)(-r.val) : (unsigned long long)r.val;
}

long long atoll(const char *nptr)
{
    return strtoll(nptr, NULL, 10);
}

intmax_t strtoimax(const char *nptr, char **endptr, int base)
{
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;
    }

    struct __strtox_result r = __strtox(nptr, base,
                                        INTMAX_MAX,
                                        (uintmax_t)INTMAX_MAX + 1);
    if (endptr) *endptr = (char *)(r.any ? r.end : nptr);
    if (r.range) errno = ERANGE;
    if (!r.any) return 0;
    return r.neg ? (intmax_t)(-r.val) : (intmax_t)r.val;
}

uintmax_t strtoumax(const char *nptr, char **endptr, int base)
{
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;
    }

    struct __strtox_result r = __strtox(nptr, base,
                                        UINTMAX_MAX, UINTMAX_MAX);
    if (endptr) *endptr = (char *)(r.any ? r.end : nptr);
    if (r.range) {
        errno = ERANGE;
        return UINTMAX_MAX;             /* sign not applied after a clamp */
    }
    if (!r.any) return 0;
    return r.neg ? (uintmax_t)(-r.val) : (uintmax_t)r.val;
}
