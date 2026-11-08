#ifndef _MOCK_BOOT_FAILURE_ARCH_IRQ_H
#define _MOCK_BOOT_FAILURE_ARCH_IRQ_H

typedef unsigned long arch_irq_state_t;
void arch_local_irq_disable(void);
void arch_local_irq_enable(void);
static inline arch_irq_state_t arch_local_irq_save(void) { return 0; }
static inline void arch_local_irq_restore(arch_irq_state_t s) { (void)s; }
uint32_t arch_irq_isa_to_gsi(uint8_t source);

#endif
