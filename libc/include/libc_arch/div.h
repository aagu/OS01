#ifndef _LIBC_ARCH_DIV_H
#define _LIBC_ARCH_DIV_H

/* Arch-neutral do_div(n, base) facade for stdio integer formatting.
 * Dispatches to the per-architecture override header (matching kernel/include/arch/). */
#ifndef do_div
#if defined(__x86_64__)
#include <libc_arch/x86_64/div.h>
#elif defined(__aarch64__)
#include <libc_arch/aarch64/div.h>
#else
#define do_div(n, base) ({ \
    unsigned long long __q = (n); \
    (n) = __q / (unsigned long)(base); \
    (unsigned long)(__q % (unsigned long)(base)); \
})
#endif
#endif /* do_div */

#endif /* _LIBC_ARCH_DIV_H */
