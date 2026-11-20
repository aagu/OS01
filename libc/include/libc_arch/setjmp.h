#ifndef _LIBC_ARCH_SETJMP_H
#define _LIBC_ARCH_SETJMP_H

/* Arch-neutral jmp_buf layout facade.
 * Dispatches to the per-architecture override header (matching kernel/include/arch/). */
#if defined(__x86_64__)
#include <libc_arch/x86_64/setjmp.h>
#elif defined(__aarch64__)
#include <libc_arch/aarch64/setjmp.h>
#else
#error "Unsupported architecture for libc setjmp facade"
#endif

#endif /* _LIBC_ARCH_SETJMP_H */
