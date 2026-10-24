// ─────────────────────────────────────────────────────────
//  arch/aarch64/intr/bad_event.c — aarch64 fatal bad-event backend
//
//  Strong definition of the arch_intr_bad_event() contract
//  (spec §5.2, kernel/include/arch/intr.h).  The common
//  dispatcher (kernel/intr/dispatch.c) reaches this function
//  when the contract is broken: a NULL event, a NULL register
//  frame, or a kind outside {INTR_SYNC, INTR_IRQ}.
//
//  Properties of this path (all mandatory):
//    • It MUST NOT dereference `event` or `regs` — both may be
//      NULL.  They are therefore not touched at all.
//    • It MUST NOT access current / per-CPU state, MUST NOT
//      call the ordinary logger, MUST NOT allocate, MUST NOT
//      take a lock.
//    • It masks interrupts on this core, prints the fixed
//      prefix and parks the CPU forever.  It never returns.
//
//  aarch64 specifics: arch_local_irq_disable() masks only the
//  I bit of DAIF (daifset #2).  The contract additionally
//  requires the FULL DAIF (D/A/F/I) to be masked, so the wider
//  "msr daifset, #0xf" follows — same two-step pattern as the
//  existing EL1h sync fatal path (arch/aarch64/intr/trap.c).
//
//  Early-reachable chain: no stack canary (the prologue would
//  need a guard reload this path cannot depend on), so the
//  definition repeats the no_stack_protector attribute.
// ─────────────────────────────────────────────────────────

#include <stdint.h>

#include <arch/cpu.h>        // arch_cpu_halt()
#include <arch/early_print.h>// arch_early_puts()
#include <arch/irq.h>        // arch_local_irq_disable()
#include <arch/intr.h>       // arch_intr_bad_event()

/* The fixed diagnostic prefix required by the contract.  Nothing
 * else is printed — in particular no argument-derived data, since
 * the arguments may be NULL. */
#define BAD_EVENT_PREFIX "[intr-dispatch] FATAL bad-event"

__attribute__((noreturn, no_stack_protector, cold))
void arch_intr_bad_event(const struct intr_event *event, pt_regs_t *regs)
{
    /* Intentionally unused: the arguments may be NULL and must
     * never be dereferenced on this path. */
    (void)event;
    (void)regs;

    /* 1. Mask IRQ, then the wider DAIF (D/A/F/I). */
    arch_local_irq_disable();
    __asm__ __volatile__("msr daifset, #0xf" ::: "memory");

    /* 2. Fixed prefix only, through the lock-free early-print
     *    facade (no logger, no lock, no allocation). */
    arch_early_puts(BAD_EVENT_PREFIX);
    arch_early_putc('\n');

    /* 3. Park the core forever (wfi).  Never returns. */
    for (;;) {
        arch_cpu_halt();
    }
}
