/* hosttests/mock/m1_a64_include/arch/spinlock.h
 *
 * Shadow <arch/spinlock.h> for the aarch64 host-test suite
 * (m1_a64_include).  The production aarch64 header
 * (kernel/include/arch/aarch64/spinlock.h) carries inline asm
 * (LDAXR / STLXR) that won't compile on the x86 host; the facade
 * (kernel/include/arch/spinlock.h) routes via <arch/x86_64/spinlock.h>
 * which uses the same instruction set — also unbuildable here.
 *
 * The test-platform shim (force-included via FRAMEWORK_INC →
 * -include mock/test_platform.h) already provides spinlock_T and the
 * full spin_init / spin_lock / spin_unlock / irqsave surface as
 * single-threaded no-ops when the `OS01_HOST_SPINLOCK_T_PROVIDED`
 * guard is set.  This shadow therefore only defines the surface if
 * test_platform.h hasn't already, so the aarch64_pt_production.o
 * link (KERNEL_INC only, no test_platform.h) still has the full
 * surface available.
 */
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H

#include <stdint.h>

#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
/* Match test_platform.h's host shadow EXACTLY — both definitions
 * must be byte-for-byte identical or a TU that includes BOTH (which
 * is the common case for the aarch64_pt_locks test) rejects the
 * second declaration.  Production aarch64 <arch/spinlock.h>'s
 * __volatile__ qualifier doesn't matter here because the host TU
 * never sees the production header. */
typedef struct
{
    unsigned long lock;
} spinlock_T;

static inline void spin_init(spinlock_T *l)          { l->lock = 1UL; }
static inline void spin_lock(spinlock_T *l)          { (void)l; }
static inline void spin_unlock(spinlock_T *l)        { (void)l; }
static inline long spin_trylock(spinlock_T *l)       { (void)l; return 1; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    (void)l;
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    (void)l;
    (void)f;
}
#endif /* OS01_HOST_SPINLOCK_T_PROVIDED */

#endif /* _ARCH_SPINLOCK_H */
