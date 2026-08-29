/* aarch64 spinlock — single-CPU-correct implementation.
 *
 * Field convention (matches x86_64 spinlock.h):
 *   lock->lock == 1  →  unlocked
 *   lock->lock == 0  →  locked
 *
 * Memory ordering:
 *   spin_lock    = ACQUIRE  on the successful CAS (arch_atomic_cas uses
 *                  LDAXR; per the ARM ARM, LDAXR/STLXR pairs are at
 *                  least ACQUIRE/RELEASE — see the atomic.h upgrade in
 *                  step 2.1).
 *   spin_unlock  = RELEASE  on the store that writes 1 back.
 *
 * We use arch_atomic_write() for the unlock store so that the RELEASE
 * comes from STLXR (paired with the LDAXR in spin_lock's CAS).  Without
 * that, a compiler that elides the STLXR (treating the store as plain)
 * would lose the synchronisation — the brief explicitly required this.
 *
 * IRQ-safe variants use arch_local_irq_save/restore() from
 * kernel/arch/irq.h (already inline for aarch64); they save the full
 * DAIF as a uint64_t and restore it verbatim.  We do NOT use any
 * x86-specific pushfq/popfq here — that would be an arch-port bug.
 */

#ifndef _ARCH_AARCH64_SPINLOCK_H
#define _ARCH_AARCH64_SPINLOCK_H

#include <stdint.h>
#include <kernel/arch/atomic.h>   /* arch_atomic_cas, arch_atomic_write,
                                   * arch_atomic_xchg */
#include <kernel/arch/irq.h>      /* arch_local_irq_save,
                                   * arch_local_irq_restore */
#include <kernel/arch/cpu.h>      /* arch_cpu_pause (yield) */

/* Match x86_64: 1=unlocked, 0=locked.  unsigned long is 64-bit on
 * aarch64 Linux/ELF, so it's the same width as uint64_t. */
typedef struct
{
    __volatile__ unsigned long lock;
} spinlock_T;

inline void __attribute__((always_inline)) spin_init(spinlock_T *lock)
{
    lock->lock = 1UL;
}

inline void __attribute__((always_inline)) spin_lock(spinlock_T *lock)
{
    /* CAS spinloop: try to flip 1 → 0.  arch_atomic_cas uses LDAXR
     * (acquire) on the load and STLXR (release) on the store, so the
     * successful CAS acts as the critical-section's ACQUIRE — any
     * prior release-store (from the matching spin_unlock) is
     * ordered-before the loads/stores that follow. */
    while (!arch_atomic_cas((volatile uint64_t *)&lock->lock, 1UL, 0UL)) {
        arch_cpu_pause();   /* `yield` — see kernel/arch/cpu.h */
    }
}

inline void __attribute__((always_inline)) spin_unlock(spinlock_T *lock)
{
    /* RELEASE store: must pair with the ACQUIRE in spin_lock.
     * arch_atomic_write() now uses STLXR (post-upgrade), so this
     * provides the release semantics.  A bare `lock->lock = 1` would
     * be relaxed on aarch64. */
    arch_atomic_write((volatile uint64_t *)&lock->lock, 1UL);
}

inline long __attribute__((always_inline)) spin_trylock(spinlock_T *lock)
{
    /* xchg unconditionally swaps and returns the previous value.  If
     * the old value was 1 (unlocked), we just took the lock.  Return
     * non-zero (= old value) on success, matching x86_64's
     * xchgq-based trylock. */
    unsigned long prev = (unsigned long)arch_atomic_xchg(
        (volatile uint64_t *)&lock->lock, 0UL);
    return (long)prev;   /* 1 if we got it, 0 if it was already locked */
}

/* Lock with IRQs disabled.  Returns the previous DAIF value, which
 * MUST be passed to spin_unlock_irqrestore() to restore IRQ state.
 * (See kernel/arch/irq.h: arch_local_irq_save returns the full DAIF
 * as uint64_t, and arch_local_irq_restore writes it back verbatim.) */
inline uint64_t __attribute__((always_inline))
spin_lock_irqsave(spinlock_T *lock)
{
    uint64_t flags = arch_local_irq_save();
    spin_lock(lock);
    return flags;
}

inline void __attribute__((always_inline))
spin_unlock_irqrestore(spinlock_T *lock, uint64_t flags)
{
    spin_unlock(lock);
    arch_local_irq_restore(flags);
}

#endif /* _ARCH_AARCH64_SPINLOCK_H */
