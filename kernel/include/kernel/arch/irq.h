#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H

#include <stdint.h>
#include <kernel/arch/thread.h>   // for pt_regs_t

// IRQ state type: 64-bit for RFLAGS (x86) and DAIF (aarch64).
// aarch64 only needs 4 bits, but uint64_t keeps the save/restore
// interface uniform and avoids truncation bugs.
typedef uint64_t arch_irq_state_t;

#ifdef __x86_64__
#include <kernel/arch/x86_64/asm.h>

static inline void arch_local_irq_enable(void)  { sti(); }
static inline void arch_local_irq_disable(void) { cli(); }

static inline arch_irq_state_t arch_local_irq_save(void) {
    arch_irq_state_t flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void arch_local_irq_restore(arch_irq_state_t flags) {
    // Use pushfq/popfq to restore ALL flags (IF, DF, AC, etc.)
    // This is the correct match for arch_local_irq_save() which
    // captures full RFLAGS via pushfq.
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

// Handler table (shared between arch and generic intr/)
typedef void (*arch_intr_handler_fn)(uint64_t nr, uint64_t param, pt_regs_t *regs);
extern arch_intr_handler_fn intr_handler_table[256];
extern void *intr_handler_param[256];

// Architecture-specific IRQ setup
void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist);
void arch_irq_install(void);

#elif defined(__aarch64__)

// DAIF: bit 7=Debug, bit 6=SError, bit 2=IRQ, bit 1=FIQ
// daifclr clears bits → enables IRQs; daifset sets bits → disables IRQs
#define DAIF_IRQ_BIT  (1UL << 2)

static inline void arch_local_irq_enable(void)
{
    __asm__ __volatile__("msr daifclr, %0" :: "i"(DAIF_IRQ_BIT) : "memory");
}

static inline void arch_local_irq_disable(void)
{
    __asm__ __volatile__("msr daifset, %0" :: "i"(DAIF_IRQ_BIT) : "memory");
}

static inline arch_irq_state_t arch_local_irq_save(void)
{
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    arch_local_irq_disable();
    return daif;
}

static inline void arch_local_irq_restore(arch_irq_state_t state)
{
    __asm__ __volatile__("msr daif, %0" :: "r"(state) : "memory");
}
#else
#error "Unsupported architecture"
#endif

#endif
