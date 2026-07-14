#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/spinlock.h>
#elif defined(__aarch64__)
#include <kernel/arch/aarch64/spinlock.h>
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_SPINLOCK_H */
