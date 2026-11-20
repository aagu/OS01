#ifndef _LIBC_ARCH_SYSCALL_H
#define _LIBC_ARCH_SYSCALL_H

/* Arch-neutral syscall trap facade.
 * Dispatches to the per-architecture override header (matching kernel/include/arch/). */
#if defined(__x86_64__)
#include <libc_arch/x86_64/syscall.h>
#elif defined(__aarch64__)
#include <libc_arch/aarch64/syscall.h>
#else
#error "Unsupported architecture for libc syscall facade"
#endif

#endif /* _LIBC_ARCH_SYSCALL_H */
