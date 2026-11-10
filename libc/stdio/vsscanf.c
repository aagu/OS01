// libc/stdio/vsscanf.c — the in-memory scanf core (libc-completion T12).
//
// The previous libc/stdio/sscanf.c was a toy: it knew %d/%u/%x/%s/%c/%%,
// had no field width, no length modifier, no %n, no %[ set, no %p and no
// float conversions, and its "unknown conversion" case silently SKIPPED
// the conversion while still walking the argument list — the classic
// "scanf consumed the wrong va_arg" bug.
//
// This file is the real thing.  It is written in three clearly separated
// parts, as the task requires:
//
//   1. FORMAT PARSING  — the conversion-spec parser (flags, field width,
//      length modifier, conversion character) and the scan-set parser.
//   2. INPUT CURSOR   — the scanners: bounded integers (T10's __strtox
//      core with a scan limit), floats (T11's __floatscan core with a
//      width-bounded field), characters, strings and scan sets.
//   3. TYPED STORE    — the stores: every conversion writes EXACTLY the
//      bytes of its target type (signed char .. intmax_t / size_t /
//      ptrdiff_t, float / double / long double, void *).
//
// Field width semantics (POSIX + glibc):
//   - the width limits the number of INPUT CHARACTERS the conversion may
//     consume; it never includes the terminating NUL of a %s / %[ field,
//   - the whitespace that precedes a numeric or string conversion is NOT
//     counted against the width (glibc: "   12345" with "%5d" yields
//     12345, not 12),
//   - a width of 0 means "no limit" (glibc's "%0d" reads the whole run),
//   - a width on %c / %s / %[ is a MAXIMUM, not a requirement: "ab" with
//     "%3[abc]" matches "ab" and succeeds.
//
// Whitespace handling (POSIX + glibc):
//   - whitespace IN THE FORMAT matches any amount of whitespace in the
//     input (including none),
//   - the numeric conversions and %s skip leading whitespace in the
//     input,
//   - %c and %[ do NOT: the 'c' item is one character and the '[' item
//     is "a sequence of characters of exactly those in the set", so a
//     leading space that is not in the set is a MATCHING FAILURE
//     (sscanf(" abc", "%[abc]", buf) == 0, and the destination is left
//     untouched).
//
// Return value (POSIX + glibc):
//   - the number of ASSIGNED conversions; %n and %* assignments do not
//     count,
//   - EOF (-1) when the input fails before the first conversion completes
//     (e.g. sscanf("", "%d", &x) == EOF),
//   - 0 on a matching failure (e.g. sscanf("abc", "%d", &x) == 0).
//
// Deliberate divergences from glibc (documented in the T12 report):
//   - an ILLEGAL length-modifier combination (%Ld, %hs, %ln, ...) stops
//     the scan explicitly and returns the count so far; glibc silently
//     ignores the modifier.  The task requires illegal combinations to be
//     rejected rather than guessed.
//   - an UNKNOWN conversion character stops the scan and returns the
//     count so far (glibc agrees).
//   - POSIX character classes ("%[[:alpha:]]") are supported; glibc's
//     scanf does not implement them.
//   - out-of-range integer results are made deterministic and sane: a
//     value whose magnitude does not fit in 64 bits CLAMPS to the target
//     type's range (glibc does the same for its 8-byte signed targets),
//     and a magnitude that fits in 64 bits but not in the target type is
//     TRUNCATED to its two's-complement low bits (glibc's answer for the
//     1/2/4-byte targets; glibc saturates its 8-byte targets instead).
//     The C standard leaves both undefined.
//
// errno: scanf never changes it (the value on entry is restored on exit).

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib/floatscan.h>
#include <stdlib/strtox.h>

/* ══════════════════════════════════════════════════════════════════
 * PART 1 — FORMAT PARSING
 * ══════════════════════════════════════════════════════════════════ */

/* the length modifier of a conversion */
enum {
    LM_NONE = 0,   /* no modifier          */
    LM_HH,         /* hh  -> signed char   */
    LM_H,          /* h   -> short          */
    LM_L,          /* l   -> long           */
    LM_LL,         /* ll  -> long long      */
    LM_J,          /* j   -> intmax_t       */
    LM_Z,          /* z   -> size_t         */
    LM_T,          /* t   -> ptrdiff_t      */
    LM_LDBL        /* L   -> long double    */
};

/* is this length modifier one of the integer ones? */
static int lm_is_integer(int lm)
{
    switch (lm) {
    case LM_NONE:
    case LM_HH: case LM_H: case LM_L: case LM_LL:
    case LM_J: case LM_Z: case LM_T:
        return 1;
    default:
        return 0;
    }
}

/* Add a POSIX character class to a scan-set bitmap.  The supported
 * classes are the ones <ctype.h> defines.  Returns 0 for an unknown
 * class name (the caller then rejects the set). */
static int scan_set_add_class(unsigned char set[32], const char *name)
{
    int lo = 0, hi = 255;
    int all = 0;

    if (!strcmp(name, "alnum"))  { all = 1; }
    else if (!strcmp(name, "alpha")) { all = 1; }
    else if (!strcmp(name, "blank")) { lo = ' '; hi = '\t'; }
    else if (!strcmp(name, "cntrl")) { lo = 0; hi = 31; }
    else if (!strcmp(name, "digit")) { lo = '0'; hi = '9'; }
    else if (!strcmp(name, "graph")) { lo = 33; hi = 126; }
    else if (!strcmp(name, "lower")) { lo = 'a'; hi = 'z'; }
    else if (!strcmp(name, "print")) { lo = 32; hi = 126; }
    else if (!strcmp(name, "punct")) { lo = 33; hi = 126; }
    else if (!strcmp(name, "space")) {
        lo = ' '; hi = ' ';
        set['\t' >> 3] |= (unsigned char)(1u << ('\t' & 7));
        set['\n' >> 3] |= (unsigned char)(1u << ('\n' & 7));
        set['\v' >> 3] |= (unsigned char)(1u << ('\v' & 7));
        set['\f' >> 3] |= (unsigned char)(1u << ('\f' & 7));
        set['\r' >> 3] |= (unsigned char)(1u << ('\r' & 7));
    }
    else if (!strcmp(name, "upper")) { lo = 'A'; hi = 'Z'; }
    else if (!strcmp(name, "xdigit")) {
        lo = '0'; hi = '9';
        set['a' >> 3] |= (unsigned char)(1u << ('a' & 7));
        set['b' >> 3] |= (unsigned char)(1u << ('b' & 7));
        set['c' >> 3] |= (unsigned char)(1u << ('c' & 7));
        set['d' >> 3] |= (unsigned char)(1u << ('d' & 7));
        set['e' >> 3] |= (unsigned char)(1u << ('e' & 7));
        set['f' >> 3] |= (unsigned char)(1u << ('f' & 7));
        set['A' >> 3] |= (unsigned char)(1u << ('A' & 7));
        set['B' >> 3] |= (unsigned char)(1u << ('B' & 7));
        set['C' >> 3] |= (unsigned char)(1u << ('C' & 7));
        set['D' >> 3] |= (unsigned char)(1u << ('D' & 7));
        set['E' >> 3] |= (unsigned char)(1u << ('E' & 7));
        set['F' >> 3] |= (unsigned char)(1u << ('F' & 7));
    }
    else
        return 0;                           /* an unknown class */

    if (all) {
        for (int c = 0; c < 256; c++) {
            int in = (c < 128) ? isalpha(c) : 0;
            if (!strcmp(name, "alnum"))
                in = in || ((c < 128) && isdigit(c));
            if (in)
                set[c >> 3] |= (unsigned char)(1u << (c & 7));
        }
        return 1;
    }
    for (int c = lo; c <= hi; c++)
        set[c >> 3] |= (unsigned char)(1u << (c & 7));
    return 1;
}

/* The scan-set parser: fills a 256-bit membership bitmap.
 *
 * The set syntax follows POSIX:
 *   %[abc]       the characters a, b and c
 *   %[^abc]      every character EXCEPT a, b and c
 *   %[]a]        a leading ']' is an ordinary character
 *   %[a-z]       a range
 *   %[[:alpha:]] a character class
 *
 * Returns 0 on a malformed set (an unterminated set, an unknown class or
 * an inverted range) — the caller then stops the scan. */
static int scan_set_parse(const char **pfmt, unsigned char set[32])
{
    const char *p = *pfmt;
    int neg = 0;
    int first = 1;

    memset(set, 0, 32);
    if (*p == '^') { neg = 1; p++; }

    for (;;) {
        if (*p == '\0')
            return 0;                       /* an unterminated set */
        if (*p == ']' && !first)
            break;                          /* the set ends here */
        first = 0;

        /* a character class: [:name:] */
        if (*p == '[' && p[1] == ':') {
            const char *q = p + 2;
            char name[24];
            int n = 0;
            while (*q != '\0' && *q != ':' && n < (int)sizeof(name) - 1)
                name[n++] = *q++;
            name[n] = '\0';
            if (q[0] == ':' && q[1] == ']') {
                if (!scan_set_add_class(set, name))
                    return 0;               /* an unknown class name */
                p = q + 2;
                continue;
            }
            /* not a class after all: '[' is an ordinary character */
        }

        {
            unsigned char lo = (unsigned char)*p;
            if (p[1] == '-' && p[2] != '\0' && p[2] != ']') {
                unsigned char hi = (unsigned char)p[2];
                if (lo > hi)
                    return 0;               /* an inverted range */
                for (int c = lo; c <= (int)hi; c++)
                    set[c >> 3] |= (unsigned char)(1u << (c & 7));
                p += 3;
            } else {
                set[lo >> 3] |= (unsigned char)(1u << (lo & 7));
                p++;
            }
        }
    }

    if (neg)
        for (int i = 0; i < 32; i++)
            set[i] = (unsigned char)~set[i];
    *pfmt = p + 1;                          /* skip the closing ']' */
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 2 — INPUT CURSOR
 * ══════════════════════════════════════════════════════════════════ */

/* the result of a float scan */
enum { FLOAT_OK = 0, FLOAT_NOMATCH, FLOAT_NOMEMORY };

/* Scan one floating-point number out of `text` (the input cursor is
 * already past the leading whitespace).
 *
 * `width` limits the number of characters the field may use: the scanner
 * then sees a NUL-terminated COPY of the first `width` characters, which
 * is exactly how a field-width-limited conversion behaves (glibc builds
 * the same kind of bounded field and converts it).
 *
 * The value is produced by T11's __floatscan core at the requested
 * precision (0 = float, 1 = double, 2 = long double), so a %Lf never
 * rounds through a double. */
static int scan_float(const char *text, int width, int prec,
                       long double *out, long *consumed)
{
    char stackbuf[96];
    char *buf = stackbuf;
    const char *field = text;
    int owned = 0;
    int status = FLOAT_OK;

    if (width > 0) {
        size_t remain = strlen(text);
        size_t len = ((size_t)width < remain) ? (size_t)width : remain;
        if (len < remain) {
            /* the width cuts the field: scan a bounded copy */
            if (len + 1 > sizeof(stackbuf)) {
                buf = malloc(len + 1);
                if (!buf)
                    return FLOAT_NOMEMORY;
                owned = 1;
            }
            memcpy(buf, text, len);
            buf[len] = '\0';
            field = buf;
        }
    }

    {
        shfile_t f;
        long double v = 0;
        long cnt = 0;
        int saved_errno = errno;

        sh_fromstring(&f, field);
        shlim(&f, 0);
        errno = 0;
        v = __floatscan(&f, prec, 1);
        if (errno == EINVAL)
            status = FLOAT_NOMATCH;         /* nothing was converted */
        errno = saved_errno;
        cnt = shcnt(&f);

        if (status == FLOAT_OK) {
            /* glibc's scanf requires the number text to be consumed
             * COMPLETELY: "1e" is a matching failure there (strtod alone
             * would happily return 1.0).  Reproduce that: a field that
             * stops right before an exponent marker is rejected. */
            if (cnt < (long)strlen(field)) {
                char stop = field[cnt];
                int numeric = (field[0] >= '0' && field[0] <= '9') ||
                              ((field[0] == '+' || field[0] == '-') &&
                               field[1] >= '0' && field[1] <= '9');
                if (numeric && (stop == 'e' || stop == 'E' ||
                                stop == 'p' || stop == 'P'))
                    status = FLOAT_NOMATCH;
            }
            if (status == FLOAT_OK) {
                *out = v;
                *consumed = cnt;
            }
        }
    }

    if (owned)
        free(buf);
    return status;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 3 — TYPED STORE
 * ══════════════════════════════════════════════════════════════════ */

/* Store an integer conversion.
 *
 * The magnitude is accumulated as an unsigned quantity and the sign is
 * applied in two's complement, so the store never overflows a signed
 * type (no UB); the result is then truncated to the width of the target
 * type, which is what glibc does for the 1/2/4-byte targets (e.g.
 * "%hhd" of "300" stores 44).
 *
 * `overflow` says that the magnitude did NOT fit in 64 bits (the digit
 * run is longer than UINTMAX_MAX).  Such a value is out of range for
 * EVERY target type, and the C standard leaves the result undefined, so
 * OS01 makes it deterministic and sane: the store CLAMPS to the target
 * type's range — the maximum for a positive value, the minimum for a
 * negative one.  That is the same answer glibc gives for its 8-byte
 * signed targets ("%lld" of "-99999999999999999999" stores LONG_MIN),
 * and it agrees with this libc's own strto* convention (T10 saturates
 * at LONG_MAX/LONG_MIN and raises ERANGE).  Without this, negating a
 * saturated magnitude would store 1 for a negative overflow — a value
 * that means nothing.
 *
 * A magnitude that DOES fit in 64 bits but not in the target type is
 * TRUNCATED (the two's-complement low bits), which is glibc's answer
 * for the 1/2/4-byte targets; glibc saturates its 8-byte targets
 * instead, and that difference is documented in the T12 report. */
static void scan_store_int(void *p, int lm, int uns, uintmax_t mag, int neg,
                           int overflow)
{
    uint64_t bits;

    if (overflow) {
        /* the value is out of range for every target: clamp */
        switch (lm) {
        case LM_HH:
            if (uns) *(unsigned char *)p = UCHAR_MAX;
            else     *(signed char *)p  = neg ? SCHAR_MIN : SCHAR_MAX;
            break;
        case LM_H:
            if (uns) *(unsigned short *)p = USHRT_MAX;
            else     *(short *)p          = neg ? SHRT_MIN : SHRT_MAX;
            break;
        case LM_L:
            if (uns) *(unsigned long *)p = ULONG_MAX;
            else     *(long *)p          = neg ? LONG_MIN : LONG_MAX;
            break;
        case LM_LL:
            if (uns) *(unsigned long long *)p = ULLONG_MAX;
            else     *(long long *)p          = neg ? LLONG_MIN : LLONG_MAX;
            break;
        case LM_J:
            if (uns) *(uintmax_t *)p = UINTMAX_MAX;
            else     *(intmax_t *)p  = neg ? INTMAX_MIN : INTMAX_MAX;
            break;
        case LM_Z:
            if (uns) *(size_t *)p = SIZE_MAX;
            else     *(ssize_t *)p = neg ? (ssize_t)PTRDIFF_MIN
                                        : (ssize_t)PTRDIFF_MAX;
            break;
        case LM_T:
            if (uns) *(size_t *)p = SIZE_MAX;
            else     *(ptrdiff_t *)p = neg ? PTRDIFF_MIN : PTRDIFF_MAX;
            break;
        default:
            if (uns) *(unsigned int *)p = UINT_MAX;
            else     *(int *)p          = neg ? INT_MIN : INT_MAX;
            break;
        }
        return;
    }

    bits = neg ? (uint64_t)(0u - (uint64_t)mag) : (uint64_t)mag;

    switch (lm) {
    case LM_HH:
        if (uns) *(unsigned char *)p = (unsigned char)bits;
        else     *(signed char *)p  = (signed char)bits;
        break;
    case LM_H:
        if (uns) *(unsigned short *)p = (unsigned short)bits;
        else     *(short *)p          = (short)bits;
        break;
    case LM_L:
        if (uns) *(unsigned long *)p = (unsigned long)bits;
        else     *(long *)p          = (long)bits;
        break;
    case LM_LL:
        if (uns) *(unsigned long long *)p = (unsigned long long)bits;
        else     *(long long *)p          = (long long)bits;
        break;
    case LM_J:
        if (uns) *(uintmax_t *)p = (uintmax_t)bits;
        else     *(intmax_t *)p  = (intmax_t)bits;
        break;
    case LM_Z:
        if (uns) *(size_t *)p = (size_t)bits;
        else     *(ssize_t *)p = (ssize_t)bits;
        break;
    case LM_T:
        if (uns) *(size_t *)p = (size_t)bits;
        else     *(ptrdiff_t *)p = (ptrdiff_t)bits;
        break;
    default:
        if (uns) *(unsigned int *)p = (unsigned int)bits;
        else     *(int *)p          = (int)bits;
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * THE SCAN LOOP
 * ══════════════════════════════════════════════════════════════════ */

int vsscanf(const char *str, const char *fmt, va_list ap)
{
    const int saved_errno = errno;
    const char *s = str;
    int count = 0;            /* the ASSIGNED conversions */
    int input_failure = 0;    /* the input ended inside a conversion */
    int stop = 0;             /* the format is finished (matched or not) */

    while (*fmt && !stop) {
        struct scan_spec {
            int  suppress;
            int  width;
            int  lm;
            char conv;
        } spec;
        unsigned char set[32];

        /* ── a literal character in the format ─────────────────── */
        if (*fmt != '%') {
            if (isspace((unsigned char)*fmt)) {
                /* whitespace in the format matches any amount of
                 * whitespace in the input (including none) */
                while (isspace((unsigned char)*fmt)) fmt++;
                while (isspace((unsigned char)*s)) s++;
                continue;
            }
            if (*s == '\0') { input_failure = 1; break; }
            if (*s != *fmt) { stop = 1; break; }  /* matching failure */
            s++; fmt++;
            continue;
        }

        /* ── a conversion ───────────────────────────────────────── */
        fmt++;                               /* the '%' */
        spec.suppress = 0;
        spec.width = 0;
        spec.lm = LM_NONE;

        if (*fmt == '*') { spec.suppress = 1; fmt++; }
        if (*fmt >= '0' && *fmt <= '9') {
            int w = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                if (w < 1000000)
                    w = w * 10 + (*fmt - '0');
                fmt++;
            }
            spec.width = w;
        }
        switch (*fmt) {
        case 'h':
            if (fmt[1] == 'h') { spec.lm = LM_HH; fmt += 2; }
            else               { spec.lm = LM_H;  fmt += 1; }
            break;
        case 'l':
            if (fmt[1] == 'l') { spec.lm = LM_LL; fmt += 2; }
            else               { spec.lm = LM_L;  fmt += 1; }
            break;
        case 'j': spec.lm = LM_J;   fmt++; break;
        case 'z': spec.lm = LM_Z;   fmt++; break;
        case 't': spec.lm = LM_T;   fmt++; break;
        case 'L': spec.lm = LM_LDBL; fmt++; break;
        default: break;
        }
        spec.conv = *fmt;
        if (spec.conv == '\0')
            break;                           /* a trailing '%' */
        fmt++;

        switch (spec.conv) {

        /* ── a literal '%' ─────────────────────────────────────── */
        case '%':
            if (*s == '\0') { input_failure = 1; break; }
            if (*s != '%') { stop = 1; break; }   /* matching failure */
            s++;
            break;

        /* ── the integer conversions ───────────────────────────── */
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int base, uns;

            if (!lm_is_integer(spec.lm)) {
                stop = 1;                    /* an illegal modifier */
                break;
            }
            switch (spec.conv) {
            case 'd': base = 10; uns = 0; break;
            case 'i': base = 0;  uns = 0; break;   /* 0x / 0 / decimal */
            case 'u': base = 10; uns = 1; break;
            case 'o': base = 8;  uns = 1; break;
            default: base = 16; uns = 1; break;   /* x, X */
            }

            while (isspace((unsigned char)*s)) s++;
            if (*s == '\0') { input_failure = 1; break; }

            {
                const char *limit = spec.width > 0 ? s + spec.width : NULL;
                struct __strtox_result r =
                    __strtox_limit(s, base, UINTMAX_MAX, UINTMAX_MAX, limit);
                if (!r.any) {
                    stop = 1;                /* matching failure */
                    break;
                }
                if (!spec.suppress) {
                    void *p = va_arg(ap, void *);
                    scan_store_int(p, spec.lm, uns, r.val, r.neg, r.range);
                    count++;
                }
                s = r.end;
            }
            break;
        }

        /* ── the floating-point conversions ────────────────────── */
        case 'a': case 'A': case 'e': case 'E':
        case 'f': case 'F': case 'g': case 'G': {
            int prec;

            if (spec.lm == LM_NONE)       prec = 0;   /* float      */
            else if (spec.lm == LM_L)    prec = 1;   /* double     */
            else if (spec.lm == LM_LDBL) prec = 2;   /* long double */
            else { stop = 1; break; }             /* illegal modifier */

            while (isspace((unsigned char)*s)) s++;
            if (*s == '\0') { input_failure = 1; break; }

            {
                long double v = 0;
                long consumed = 0;
                int st = scan_float(s, spec.width, prec, &v, &consumed);
                if (st == FLOAT_NOMEMORY) { stop = 1; break; }
                if (st == FLOAT_NOMATCH) { stop = 1; break; }
                if (!spec.suppress) {
                    void *p = va_arg(ap, void *);
                    if (prec == 0)      *(float *)p = (float)v;
                    else if (prec == 1) *(double *)p = (double)v;
                    else                *(long double *)p = v;
                    count++;
                }
                s += consumed;
            }
            break;
        }

        /* ── %c: characters, no whitespace skipping, no NUL ─────── */
        case 'c': {
            int n = spec.width > 0 ? spec.width : 1;
            void *p = NULL;

            if (spec.lm != LM_NONE) {
                stop = 1;                    /* an illegal modifier */
                break;
            }
            if (!spec.suppress)
                p = va_arg(ap, void *);
            for (int i = 0; i < n; i++) {
                if (*s == '\0') { input_failure = 1; break; }
                if (p) ((char *)p)[i] = *s;
                s++;
            }
            if (!input_failure && !spec.suppress)
                count++;
            break;
        }

        /* ── %s: a whitespace-delimited token, NUL-terminated ──── */
        case 's': {
            void *p = NULL;

            if (spec.lm != LM_NONE) {
                stop = 1;                    /* an illegal modifier */
                break;
            }
            while (isspace((unsigned char)*s)) s++;
            if (*s == '\0') { input_failure = 1; break; }
            if (!spec.suppress)
                p = va_arg(ap, void *);
            {
                int i = 0;
                while (*s != '\0' && !isspace((unsigned char)*s)) {
                    if (spec.width > 0 && i >= spec.width)
                        break;
                    if (p) ((char *)p)[i] = *s;
                    i++; s++;
                }
                if (p) ((char *)p)[i] = '\0';
            }
            if (!spec.suppress)
                count++;
            break;
        }

        /* ── %[ : a scan set, NUL-terminated ───────────────────── */
        case '[': {
            void *p = NULL;

            if (spec.lm != LM_NONE) {
                stop = 1;                    /* an illegal modifier */
                break;
            }
            if (!scan_set_parse(&fmt, set)) {
                stop = 1;                    /* a malformed set */
                break;
            }
            /* NO whitespace skipping here: the '[' input item is "a
             * sequence of characters of exactly those in the set", so a
             * leading space that is not in the set is a MATCHING
             * FAILURE (POSIX and glibc both behave this way; only %s
             * skips leading whitespace).  A space that IS a member of
             * the set is matched and stored like any other member. */
            if (*s == '\0') { input_failure = 1; break; }
            if (!spec.suppress)
                p = va_arg(ap, void *);
            {
                int i = 0;
                int matched = 0;
                while (*s != '\0') {
                    unsigned char c = (unsigned char)*s;
                    if (!((set[c >> 3] >> (c & 7)) & 1))
                        break;
                    if (spec.width > 0 && i >= spec.width)
                        break;
                    if (p) ((char *)p)[i] = (char)c;
                    i++; s++; matched = 1;
                }
                if (!matched) {
                    stop = 1;                /* matching failure */
                    break;
                }
                if (p) ((char *)p)[i] = '\0';
            }
            if (!spec.suppress)
                count++;
            break;
        }

        /* ── %p : a pointer ────────────────────────────────────── */
        case 'p': {
            void *p = NULL;

            if (spec.lm != LM_NONE) {
                stop = 1;                    /* an illegal modifier */
                break;
            }
            while (isspace((unsigned char)*s)) s++;
            if (*s == '\0') { input_failure = 1; break; }
            {
                const char *limit = spec.width > 0 ? s + spec.width : NULL;
                struct __strtox_result r =
                    __strtox_limit(s, 16, UINTMAX_MAX, UINTMAX_MAX, limit);
                if (!r.any) {
                    stop = 1;                /* matching failure */
                    break;
                }
                /* "0x" with no hex digit behind it is not a number */
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
                    r.end == s + 1) {
                    stop = 1;                /* matching failure */
                    break;
                }
                if (!spec.suppress) {
                    p = va_arg(ap, void *);
                    *(void **)p = (void *)(uintptr_t)r.val;
                    count++;
                }
                s = r.end;
            }
            break;
        }

        /* ── %n : the number of characters consumed so far ──────── */
        case 'n':
            if (spec.lm != LM_NONE || spec.width > 0) {
                stop = 1;                    /* an illegal modifier */
                break;
            }
            if (!spec.suppress) {
                int *p = va_arg(ap, int *);
                *p = (int)(s - str);
            }
            break;                           /* %n does not count */

        /* ── an unknown conversion: stop the scan ──────────────── */
        default:
            stop = 1;
            break;
        }

        if (input_failure)
            break;
    }

    errno = saved_errno;
    if (input_failure && count == 0)
        return EOF;                         /* the input failed first */
    return count;
}
