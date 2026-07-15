// aarch64 spinlock stub — not yet implemented.
// x86_64 uses the lock prefix; aarch64 will use ldxr/stlxr exclusive stores.
// For now, any aarch64 code that tries to use spinlocks will hit this error.
#ifndef _ARCH_AARCH64_SPINLOCK_H
#define _ARCH_AARCH64_SPINLOCK_H
#error "aarch64 spinlock.h not yet implemented"
#endif
