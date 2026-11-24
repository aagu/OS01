#ifndef _MOCK_ASLR_EXEC_ARCH_IRQ_H
#define _MOCK_ASLR_EXEC_ARCH_IRQ_H

/*
 * Isolated <arch/irq.h> for the x86_64 ASLR spawn/exec host test.
 *
 * The exec commit window must run irq_save -> CR3 switch -> FS install
 * -> irq_restore.  The production header's OS01_HOST_TEST branch is a
 * no-op, so this mock routes the mask transitions to test-owned
 * recorders instead.  Only test_x86_aslr_exec links against it (the
 * -I$(TEST_MOCK)/aslr_exec directory is private to that test), and the
 * test provides both strong definitions.
 */

#include <stdint.h>

typedef unsigned long arch_irq_state_t;

void arch_local_irq_disable(void);
void arch_local_irq_enable(void);

arch_irq_state_t arch_host_test_irq_save(void);
void arch_host_test_irq_restore(arch_irq_state_t flags);

static inline arch_irq_state_t arch_local_irq_save(void)
{
    return arch_host_test_irq_save();
}

static inline void arch_local_irq_restore(arch_irq_state_t flags)
{
    arch_host_test_irq_restore(flags);
}

uint32_t arch_irq_isa_to_gsi(uint8_t source);

#endif /* _MOCK_ASLR_EXEC_ARCH_IRQ_H */
