#ifndef _STDLIB_STRTOX_H
#define _STDLIB_STRTOX_H 1

/*
 * libc/include/stdlib/strtox.h — the PRIVATE integer-conversion core
 * (libc-completion plan T10).
 *
 * One core serves every strto* spelling.  The plan's contract: all
 * arithmetic in UNSIGNED (so saturating at LONG_MIN's magnitude never
 * negates a signed value — no UB), one sign/base/overflow code path,
 * and endptr/errno discipline decided by the wrappers.
 *
 * Core contract (__strtox):
 *   - skips isspace, reads an optional sign, detects the base
 *     (explicit 2..36, or 0 = auto: 0x → 16, leading 0 → 8, else 10),
 *   - consumes "0x"/"0X" ONLY if a hex digit follows (so "0x" alone
 *     parses as "0" with endptr after the '0', per POSIX),
 *   - accumulates the magnitude in uintmax_t with the cap chosen by
 *     the sign: max_pos for '+', max_neg for '-'.  On overflow the
 *     accumulator saturates at the cap but scanning CONTINUES to the
 *     end of the digit run (POSIX: endptr must be the last possible
 *     position even on ERANGE),
 *   - any=1 iff at least one digit was consumed; end is the first
 *     unconsumed character (nptr itself when no digits),
 *   - range=1 iff the digit run exceeded the applied cap.
 *
 * The base VALIDATION (2..36 or 0) is the wrapper's job: it answers
 * 0 / endptr=nptr / errno=EINVAL without touching the core.
 */

#include <stdint.h>

struct __strtox_result {
    uintmax_t   val;    /* magnitude, saturated to the applied cap */
    const char *end;    /* first unconsumed character */
    int         neg;    /* a '-' was seen */
    int         any;    /* at least one digit consumed */
    int         range;  /* the digit run overflowed the cap */
};

struct __strtox_result __strtox(const char *nptr, int base,
                                uintmax_t max_pos, uintmax_t max_neg);

/* The BOUNDED spelling (T12): the scan stops at `limit` (exclusive) as
 * well as at the terminating NUL.  It exists for the field-width
 * limited conversions of scanf, whose number must stop at the width
 * instead of running to the end of the digit run.  `limit` bounds the
 * whitespace skip, the sign and the "0x" prefix probe too, so a bounded
 * field never reads past its own end.  limit == NULL is exactly
 * __strtox(). */
struct __strtox_result __strtox_limit(const char *nptr, int base,
                                       uintmax_t max_pos, uintmax_t max_neg,
                                       const char *limit);

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_STRTOX_H */
