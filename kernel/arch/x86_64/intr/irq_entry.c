/* Native IRQ landing points. Validation precedes any frame access. */
#include <arch/irq.h>
#include <arch/intr.h>
#include <arch/x86_64/intr_decode.h>
#include <arch/x86_64/intr_dispatch.h>
#include <arch/x86_64/intr_vectors.h>
#include <intr/vector_dispatch.h>

__attribute__((no_stack_protector))
static void x86_dispatch_irq(pt_regs_t *regs, uint64_t vector)
{
    struct intr_event event;
    x86_intr_decode_irq(&event, regs, vector);
    arch_intr_dispatch(&event, regs);
}

__attribute__((no_stack_protector))
void arch_irq_dispatch(pt_regs_t *regs, uint64_t vector)
{
    if (!regs || vector < X86_VECTOR_DEVICE_FIRST || vector > X86_VECTOR_DEVICE_LAST)
        arch_intr_bad_event(0, regs);
    x86_dispatch_irq(regs, vector);
}

__attribute__((no_stack_protector))
void generic_intr_dispatch(pt_regs_t *regs, uint64_t vector)
{
    if (!regs || (vector != X86_VECTOR_TLB && vector != X86_VECTOR_RESCHED))
        arch_intr_bad_event(0, regs);
    x86_dispatch_irq(regs, vector);
}

__attribute__((no_stack_protector))
void x86_irq_entry(pt_regs_t *regs, uint64_t vector)
{
    if (!regs || (vector != X86_VECTOR_LAPIC_TIMER && vector != X86_VECTOR_SPURIOUS))
        arch_intr_bad_event(0, regs);
    x86_dispatch_irq(regs, vector);
}

__attribute__((no_stack_protector))
void x86_handle_registered_irq(pt_regs_t *regs, uint64_t vector)
{
    intr_dispatch_registered_vector(regs, vector);
}

__attribute__((no_stack_protector))
void x86_handle_spurious_irq(pt_regs_t *regs, uint64_t vector)
{
    (void)regs;
    (void)vector;
}
