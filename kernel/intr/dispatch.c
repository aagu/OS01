// ─────────────────────────────────────────────────────────
//  intr/dispatch.c — common stateless event dispatcher
//
//  Design spec §5.2.  One arch-neutral entry point that every
//  architecture's entry adapter calls:
//
//      arch_intr_dispatch(event, regs)
//
//  The dispatcher is STATELESS: it owns no lock, no allocation,
//  no per-CPU slot, no IRQ state to restore and no cleanup to
//  perform.  A backend may therefore escape non-locally (an
//  x86 uaccess longjmp, a task exit, a fatal halt) without
//  leaking anything — the common chain holds nothing that
//  requires a normal return to release.
//
//  Validation is deliberately minimal and is the ONLY thing
//  this file inspects:
//      • event must not be NULL,
//      • regs must not be NULL,
//      • event->kind must be one of the two legal values.
//
//  No other event member is read and NO pt_regs_t member is
//  read — the register frame is only a pointer that is handed
//  to the architecture backend untouched.  Everything else
//  (native payload consistency, origin whitelist, …) belongs
//  to the architecture backends.
//
//  Early-reachable chain constraints (spec §5.2 / §9.1): no
//  allocation, no locks, no ordinary logger, no current /
//  per-CPU access, no stack-canary dependency, no boot-state
//  gate.  The definition therefore repeats the
//  no_stack_protector attribute of the declaration in
//  intr/dispatch.h.
// ─────────────────────────────────────────────────────────

#include <stddef.h>   // NULL

#include <arch/intr.h>
#include <intr/dispatch.h>

__attribute__((no_stack_protector))
void arch_intr_dispatch(const struct intr_event *event, pt_regs_t *regs)
{
    /* A NULL event or a NULL register frame is a broken contract, not a
     * recoverable fault: it reaches the fatal bad-event backend. */
    if (event != NULL && regs != NULL) {
        /* Only the kind is inspected.  It is read through a plain int so
         * that no enum-range assumption can fold the fallback away: the
         * contract explicitly covers an out-of-range kind. */
        int kind = (int)event->kind;

        if (kind == (int)INTR_SYNC) {
            arch_intr_handle_sync(event, regs);
            return;
        }

        if (kind == (int)INTR_IRQ) {
            arch_intr_handle_irq(event, regs);
            return;
        }
    }

    /* NULL event, NULL regs, or an out-of-range kind.  The backend never
     * returns, so the dispatcher has no tail work of its own. */
    arch_intr_bad_event(event, regs);
}
