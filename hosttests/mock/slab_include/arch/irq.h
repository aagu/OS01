/* test/mock/slab_include/arch/irq.h — host shadow of
 * kernel/include/arch/irq.h for compiling the REAL
 * kernel/memory/slab.c in the host test suite (test_slab_idempotent_reservation,
 * test_slab_basic_x86_count).
 *
 * The production arch_local_irq_save / arch_local_irq_restore execute
 * `pushfq; popq <reg>; cli` and `pushq <reg>; popfq` inline asm —
 * privileged instructions that fault in a host userspace process. This
 * shadow keeps the type and function surface slab.c consumes
 * (arch_irq_state_t + arch_local_irq_save / arch_local_irq_restore)
 * but turns both into no-op stubs that round-trip a 0 state. The host
 * suite is single-threaded so the actual interrupt flag is never
 * consulted.
 *
 * Only the slab-c specific subset is shadowed; the production irq.h
 * exposes many more symbols (hw_int_controller_t dispatch, IDT setup,
 * GIC controller hooks, ...) that slab.c does not reference. If a
 * future host test needs any of those, the shadow must be extended.
 *
 * This directory is placed BEFORE kernel/include on the include path
 * (SLAB_SHADOW_INC in test/Makefile) so only this header is shadowed;
 * every other <kernel/...> include resolves to the production header.
 */
#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H

#include <stdint.h>

typedef uint64_t arch_irq_state_t;

/* No-op save/restore: returns 0, restores 0. The host suite never
 * consults the saved state, so the value is load-bearing only for
 * type compatibility. */
static inline arch_irq_state_t arch_local_irq_save(void)
{
    return (arch_irq_state_t)0;
}

static inline void arch_local_irq_restore(arch_irq_state_t flags)
{
    (void)flags;
}

#endif /* _ARCH_IRQ_H */