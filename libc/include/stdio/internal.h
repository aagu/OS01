#ifndef _STDIO_INTERNAL_H
#define _STDIO_INTERNAL_H 1

/*
 * libc/include/stdio/internal.h — the PRIVATE stdio implementation
 * header.
 *
 * This file used to live at libc/stdio/stdio_internal.h.  It moved here
 * so that every private libc header sits under libc/include/<module>/,
 * next to the public header it belongs to.  There is exactly ONE copy of
 * this content: the old path was deleted, not kept in sync.
 *
 * Nothing declared here is part of the public libc interface.
 */

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

/* The stream state and the internal resolver live in the sibling
 * private header; every stdio translation unit needs both. */
#include <stdio/file.h>

/* ── The printf format flags ───────────────────────────────────────
 *
 * These flags are the libc's IMPLEMENTATION detail: they describe the
 * parsed format string of one conversion, not the public interface of the
 * C library.  They used to live in the public <stdio.h>, where they
 * polluted the namespace of every consumer: a caller with an enum member
 * named LEFT (or a variable named SIGN) could not even include the
 * header any more.  They belong here, next to the formatter that
 * interprets them.
 *
 * Every definition is guarded: the host-test platform stub
 * (hosttests/mock/test_platform.h) provides the same names for the kernel
 * sources it compiles, and the two must never fight over them. */
#ifndef ZEROPAD
#define ZEROPAD	1		/* pad with zero */
#endif
#ifndef SIGN
#define SIGN	2		/* unsigned/signed long */
#endif
#ifndef PLUS
#define PLUS	4		/* show plus */
#endif
#ifndef SPACE
#define SPACE	8		/* space if plus */
#endif
#ifndef LEFT
#define LEFT	16		/* left justified */
#endif
#ifndef SPECIAL
#define SPECIAL	32		/* 0x */
#endif
#ifndef SMALL
#define SMALL	64		/* use 'abcdef' instead of 'ABCDEF' */
#endif

/* do_div: per-arch inline asm.
 *   x86_64 : divq — single instruction, quotient in rax, remainder in rdx.
 *   aarch64: udiv + msub — two instructions, quotient via udiv, remainder
 *            via msub = dividend - quotient*divisor. The dividend is read
 *            twice (udiv's source operand Xn + msub's source operand Xa);
 *            no constraint tying is used, so the compiler doesn't merge
 *            those reads — each asm instruction re-loads __qn.
 *   else   : portable C fallback.
 *
 * libc/stdio is x86_64-only on the libc userland side (the Makefile
 * gates libc.a to x86_64), so the asm path covers both x86_64 libc
 * userland and x86_64 libk kernel-side builds. */
#ifndef do_div
#if defined(__x86_64__)
#define do_div(n,base) ({ \
	int __res; \
	__asm__("divq %%rcx":"=a" (n),"=d" (__res):"0" (n),"1" (0),"c" (base)); \
	__res; })
#elif defined(__aarch64__)
#define do_div(n,base) ({ \
    unsigned long __b = (unsigned long)(base); \
    unsigned long long __qn = (unsigned long long)(n); \
    unsigned long long __q; \
    unsigned long __rem; \
    __asm__ volatile ("udiv %0, %1, %2" \
                      : "=r"(__q) \
                      : "r"(__qn), "r"(__b)); \
    __asm__ volatile ("msub %0, %3, %1, %2" \
                      : "=r"(__rem) \
                      : "r"(__q), "r"(__qn), "r"(__b)); \
    (n) = __q; \
    __rem; })
#else
#define do_div(n,base) ({ \
    unsigned long long __q = (n); \
    (n) = __q / (unsigned long)(base); \
    (unsigned long)(__q % (unsigned long)(base)); \
})
#endif
#endif /* do_div */

#ifndef is_digit
#define is_digit(c)	((c) >= '0' && (c) <= '9')
#endif

/*
 * Bounded core formatter. Internal to the stdio implementation; not part of
 * the public libc API.
 *
 * cap          : full size of the destination array, INCLUDING the NUL.
 * perform_assign: gates ONLY the %n pointer write. When 0, %n still consumes
 *                 its va_arg but does not dereference/store. Every conversion
 *                 unconditionally consumes its va_arg regardless of this flag.
 *
 * Returns the full would-be length (C99 snprintf semantics). Returns SIZE_MAX
 * if the internal counter overflows.
 */
size_t vformatter(char *dst, size_t cap, const char *fmt, va_list ap, int perform_assign);

/* Length-safe write: returns len on success, -1 on error. write_all(…, 0)
 * succeeds immediately. Retries only on EINTR; a zero write while bytes remain
 * and all other errors return -1. Never writes past the requested length.
 *
 * ssize_t comes from <sys/types.h>, which both arches include via
 * <stdio.h> (transitive). The actual definition in stdio_file.c is
 * only linked on x86_64 userland libc; libk-mode aarch64 doesn't pull
 * it in. The decl stays here so printf.c (libk-compiled on x86_64)
 * can call it. */
ssize_t write_all(int fd, const char *buf, size_t len);

#endif /* _STDIO_INTERNAL_H */
