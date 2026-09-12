/* test/mock/pmm_include/kernel/arch/spinlock.h — host shadow of
 * kernel/arch/spinlock.h for compiling the REAL kernel/memory/pmm.c
 * in the host test suite (test_pmm_ram_rel_index).
 *
 * The production x86_64 spinlock uses `lock decq` inline asm and the
 * irqsave variants execute cli/sti — privileged instructions that
 * fault in a host userspace process. This shadow keeps the exact
 * type/function surface pmm.c consumes but makes every operation a
 * no-op: the host suite is single-threaded.
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
