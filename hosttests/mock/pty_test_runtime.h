/*
 * hosttests/mock/pty_test_runtime.h
 *
 * Mock runtime environment for PTY host testing (Task 6).
 * Provides thread-safe spinlocks following OS01 convention (1 = unlocked, 0 = locked).
 */
#ifndef OS01_PTY_TEST_RUNTIME_H
#define OS01_PTY_TEST_RUNTIME_H

#define OS01_HOST_SPINLOCK_T_PROVIDED 1
#define __SPINLOCK_H__ 1
#define _ARCH_SPINLOCK_H 1

#include <stdint.h>
#include <stdbool.h>

/* ── Thread-safe spinlock matching OS01 convention (1 = unlocked, 0 = locked) ── */
typedef struct {
    volatile long lock;
} spinlock_T;

static inline void spin_init(spinlock_T *l) {
    __atomic_store_n(&l->lock, 1L, __ATOMIC_RELEASE);
}

static inline void spin_lock(spinlock_T *l) {
    while (1) {
        long expected = 1L;
        if (__atomic_compare_exchange_n(&l->lock, &expected, 0L, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}

static inline void spin_unlock(spinlock_T *l) {
    __atomic_store_n(&l->lock, 1L, __ATOMIC_RELEASE);
}

static inline long spin_trylock(spinlock_T *l) {
    long expected = 1L;
    return __atomic_compare_exchange_n(&l->lock, &expected, 0L, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 1L : 0L;
}

static inline uint64_t spin_lock_irqsave(spinlock_T *l) {
    spin_lock(l);
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) {
    (void)f;
    spin_unlock(l);
}

#include "gfx_test_runtime.h"

#endif /* OS01_PTY_TEST_RUNTIME_H */
