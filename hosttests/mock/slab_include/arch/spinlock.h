/* test/mock/slab_include/arch/spinlock.h — host shadow of
 * kernel/include/arch/spinlock.h for compiling the REAL
 * kernel/memory/slab.c in the host test suite (test_slab_idempotent_reservation,
 * test_slab_basic_x86_count).
 *
 * The production x86_64 spinlock uses `lock decq` inline asm and the
 * irqsave variants execute cli/sti — privileged instructions that
 * fault in a host userspace process. This shadow keeps the exact
 * type/function surface slab.c consumes (slab_lock_acquire /
 * slab_lock_release wrap spin_lock_irqsave / spin_unlock_irqrestore-
 * style state but only call spin_lock / spin_unlock directly) but
 * makes every operation a no-op: the host suite is single-threaded
 * and slab.c's online branch (percpu_data[0].online) is false on host,
 * so the spin_lock / spin_unlock calls never run anyway.
 *
 * This directory is placed BEFORE kernel/include on the include path
 * (SLAB_SHADOW_INC in test/Makefile) so only this header is shadowed;
 * every other <kernel/...> include resolves to the production header.
 */
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H

#include <stdint.h>

typedef struct
{
    __volatile__ unsigned long lock;   /* 1: unlock, 0: lock */
} spinlock_T;

static inline void spin_init(spinlock_T *lock)      { lock->lock = 1L; }
static inline void spin_lock(spinlock_T *lock)      { (void)lock; }
static inline void spin_unlock(spinlock_T *lock)    { lock->lock = 1L; }
static inline long spin_trylock(spinlock_T *lock)   { (void)lock; return 1; }

static inline uint64_t spin_lock_irqsave(spinlock_T *lock)
{
    (void)lock;
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_T *lock, uint64_t flags)
{
    (void)lock;
    (void)flags;
}

#endif /* _ARCH_SPINLOCK_H */