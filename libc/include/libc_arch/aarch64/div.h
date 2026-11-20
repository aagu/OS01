#ifndef _LIBC_ARCH_AARCH64_DIV_H
#define _LIBC_ARCH_AARCH64_DIV_H

/* aarch64: udiv + msub — quotient via udiv, remainder via msub = dividend - quotient*divisor.
 * No constraint tying is used so each instruction re-loads __qn cleanly. */
#define do_div(n, base) ({ \
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

#endif /* _LIBC_ARCH_AARCH64_DIV_H */
