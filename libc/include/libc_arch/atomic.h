#ifndef _LIBC_ARCH_ATOMIC_H
#define _LIBC_ARCH_ATOMIC_H

/* Arch-neutral 32-bit atomic facade for libc synchronization primitives.
 * Dispatches to the per-architecture override header (matching kernel/include/arch/). */
#if defined(__x86_64__)
#include <libc_arch/x86_64/atomic.h>
#elif defined(__aarch64__)
#include <libc_arch/aarch64/atomic.h>
#else
#error "Unsupported architecture for libc atomic facade"
#endif

#endif /* _LIBC_ARCH_ATOMIC_H */
