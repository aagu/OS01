#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H

#include <stdint.h>
#include <arch/regs.h>   // for pt_regs_t (via the arch-neutral facade)

// IRQ state type: 64-bit for RFLAGS (x86) and DAIF (aarch64).
// aarch64 only needs 4 bits, but uint64_t keeps the save/restore
// interface uniform and avoids truncation bugs.
typedef uint64_t arch_irq_state_t;

// ── Arch-neutral IRQ dispatch hooks ─────────────────────
//
// Three hooks let kernel/intr/irq.c stay free of APIC / PIC / GIC
// names. Each arch provides a strong override; weak defaults panic
// or identity-map. See:
//
//   • weak defaults:          kernel/intr/arch_irq_hooks.c
//   • x86_64 strong override: kernel/arch/x86_64/irq_hooks.c
//
// Forward decl avoids circular include — the full type lives in
// kernel/include/kernel/interrupt.h, which already includes this
// header.
struct hw_int_type;
typedef struct hw_int_type hw_int_controller_t;

// Returns the hw_int_controller_t that routes this GSI on the current
// arch, or NULL if no controller is available (caller aborts).
//   x86_64:  IOAPIC if apic_available(), else legacy PIC for gsi<16.
//   aarch64: GIC v2/v3 (TBD).
hw_int_controller_t *arch_irq_select_controller(uint32_t gsi);

// Translate between the kernel's GSI number and the arch's notion of
// "vector" / "hwirq" (whatever the exception-vector stub passes to
// the dispatch hook). On x86_64 the IDT maps hwirq = 0x20 + gsi,
// so gsi == 0 → vector 0x20 (the master PIC's IRQ0). On aarch64
// GIC, hwirq == INTID == gsi (identity). Default: identity.
uint64_t arch_irq_gsi_to_vector(uint32_t gsi);
uint32_t arch_irq_vector_to_gsi(uint64_t vector);

// Hardware-IRQ dispatch entry point. Called from each arch's
// exception-vector stub with the raw hwirq value read from the
// interrupt controller. Looks up the GSI, invokes the registered
// handler, ACKs the controller. x86_64's IDT stub calls this with
// the IDT vector (e.g. 0x20 + gsi); aarch64's vbar stub will call
// it with the GIC INTID.
void arch_irq_dispatch(pt_regs_t *regs, uint64_t hwirq);

#ifdef __x86_64__
#include <arch/x86_64/asm.h>

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

// daifclr/daifset take a 4-bit immediate laid out {D, A, I, F}:
//   imm[3]=D(Debug), imm[2]=A(SError), imm[1]=I(IRQ), imm[0]=F(FIQ).
// So IRQ is bit 1 of the immediate (value 2), NOT bit 2.  A value of
// 4 (=1<<2) targets the SError mask, leaving IRQ permanently masked —
// that is the classic "timer IRQ never fires" bug.  The `mrs/msr daif`
// register (used by save/restore) has a DIFFERENT layout (bit7=I); the
// two must not be conflated.
#define DAIF_IRQ_BIT  (1UL << 1)

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
