/* test/mock/pmm_include/kernel/arch/spinlock.h — host shadow of
 * kernel/arch/spinlock.h for compiling the REAL kernel/memory/pmm.c
 * in the host test suite (test_pmm_ram_rel_index).
 *
 * The production x86_64 spinlock uses `lock decq` inline asm and the
 * irqsave variants execute cli/sti — privileged instructions that
 * fault in a host userspace process. This shadow keeps the exact
 * type/function surface pmm.c consumes but makes every operation a
 * no-op: the host suite is single-threaded. Optional irqsave observers
 * let a fixture expose writer transitions at lock boundaries without
 * adding hooks to production code or claiming real mutual exclusion.
 *
 * This directory is placed BEFORE kernel/include on the include path
 * (PMM_HOST_CFLAGS in test/Makefile) so only this header is shadowed;
 * every other <kernel/...> include resolves to the production header.
 */
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H

#include <stdint.h>

typedef struct
{
    __volatile__ unsigned long lock;   /* 1: unlock, 0: lock */
} spinlock_T;

void pmm_test_spinlock_acquire(spinlock_T *lock) __attribute__((weak));
void pmm_test_spinlock_release(spinlock_T *lock) __attribute__((weak));

static inline void spin_init(spinlock_T *lock)      { lock->lock = 1L; }
static inline void spin_lock(spinlock_T *lock)      { (void)lock; }
static inline void spin_unlock(spinlock_T *lock)    { lock->lock = 1L; }
static inline long spin_trylock(spinlock_T *lock)   { (void)lock; return 1; }

static inline uint64_t spin_lock_irqsave(spinlock_T *lock)
{
    if (pmm_test_spinlock_acquire)
        pmm_test_spinlock_acquire(lock);
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_T *lock, uint64_t flags)
{
    if (pmm_test_spinlock_release)
        pmm_test_spinlock_release(lock);
    (void)flags;
}

#endif /* _ARCH_SPINLOCK_H */
