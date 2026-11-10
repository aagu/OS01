#ifndef _STDLIB_FLOATSCAN_H
#define _STDLIB_FLOATSCAN_H 1

/*
 * libc/include/stdlib/floatscan.h — the PRIVATE float scanner surface
 * (libc-completion plan T11, porting musl 1.2.5).
 *
 * musl's floatscan consumes its shgetc "scan helper stdio": a FILE*
 * that may either be a real stdio stream or a STRING PSEUDO-FILE whose
 * buffer pointers point into the string (sh_fromstring).  OS01's
 * strtod family only ever scans memory strings, so this port keeps
 * ONLY the string pseudo-FILE: the struct below carries exactly the
 * fields musl's sh macros touch, __uflow and the whole FILE
 * implementation are NOT imported, and shgetc answers EOF sticky once
 * the terminating NUL is reached (EOF reads do not advance, which
 * keeps musl's shunget bookkeeping balanced).
 *
 * Position contract (what strtod's wrappers rely on, identical to
 * musl's): shcnt(f) is the net character offset — every shgetc
 * advances (including the single terminating-NUL read), every
 * shunget steps back — and after __floatscan returns, shcnt is
 * EXACTLY the POSIX endptr offset.  The wrapper reads it WITHOUT a
 * compensating shunget, exactly like musl's src/stdlib/strtod.c
 * strtox().
 */

#include <stddef.h>
#include <sys/types.h>          /* off_t */

typedef struct os01_shfile {
    const char *buf;            /* start of the string */
    const char *rpos;           /* next character to read */
    long shlim;                 /* always 0 here (memory input: no limit) */
} shfile_t;

#define sh_fromstring(f, s) ((void)((f)->buf = (f)->rpos = (s)), \
                             (void)((f)->shlim = 0))

#define shcnt(f)  ((long)((f)->rpos - (f)->buf))
#define shunget(f) ((void)((f)->rpos--))

/* musl's string-pseudo-FILE shgetc, verbatim semantics: reads the
 * character AT rpos and advances — including the terminating NUL,
 * exactly once (every scanner loop exits on '\0', and musl's unget
 * bookkeeping then lands the position precisely on the POSIX endptr;
 * rpos may point one past the terminator but is never DEREFERENCED
 * there).  Do NOT "fix" this to stop at NUL — musl's strtod wrapper
 * reads shcnt() without a compensating shunget, and the whole
 * position contract depends on this advancing read. */
static inline int shgetc(shfile_t *f)
{
    unsigned char c = (unsigned char)*f->rpos;
    f->rpos++;
    return c;
}

#define shlim(f, lim) ((void)((f)->shlim = (lim)))

#ifdef __cplusplus
extern "C" {
#endif

/* Scan one floating-point number from the string pseudo-file.  prec:
 * 0 = float, 1 = double, 2 = long double (the accuracy target — the
 * scanner never rounds through a narrower type than requested).  pok:
 * accept partial matches ("1e+", "infinity", "nan(" without the
 * closing parenthesis), which is what strtod needs.  Returns the
 * value (long double; the caller narrows), sets errno = ERANGE on
 * overflow/underflow. */
long double __floatscan(shfile_t *f, int prec, int pok);

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_FLOATSCAN_H */
