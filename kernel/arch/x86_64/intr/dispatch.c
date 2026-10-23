// ─────────────────────────────────────────────────────────
//  arch/x86_64/intr/dispatch.c — x86_64 routing backend
//
//  Design spec §5.2 / §6.1.  The STRONG per-architecture
//  definitions the common dispatcher (kernel/intr/dispatch.c)
//  links against:
//
//    arch_intr_handle_sync() — deterministic re-validation, then
//        an EXPLICIT four-case switch on the native vector that
//        calls the corresponding private business handler in
//        trap.c EXACTLY once, with the ORIGINAL error code and
//        (for #PF) the ORIGINAL CR2 snapshot.  Every other
//        vector is unmigrated in phase one and must FAIL CLOSED
//        through arch_intr_bad_event().
//    arch_intr_handle_irq()  — fail-closed ONLY (spec §5.2):
//        no production caller exists in phase one, and it must
//        never route to arch_irq_dispatch() pretending the IRQ
//        surface is covered.  It exists so the strong-link
//        contract holds.
//
//  Deterministic re-validation (plan "重解码纯载荷并逐成员比较"):
//  the backend re-decodes the pure payload from the event's OWN
//  raw fields — vector = native_vector, error_code =
//  native_status, cr2 = native_address — and compares the result
//  MEMBER BY MEMBER with the event it was handed.  struct
//  intr_event has padding, so memcmp is never used.  Any
//  disagreement means the contract is broken → bad-event.
//
//  Origin rules (spec §6.1):
//    • the origin comes from the saved CS privilege level and the
//      #PF error-code U/S bit is deliberately NOT cross-checked
//      against it — the kernel legitimately faults on user
//      addresses, so the two are independent;
//    • INTR_ORIGIN_UNKNOWN (CPL 1/2) is a broken contract for a
//      migrated entry → bad-event.
//
//  Unlike the common dispatcher, this backend MAY read pt_regs_t
//  members (the decoder needs the saved CS).
// ─────────────────────────────────────────────────────────

#include <stdbool.h>

#include <arch/regs.h>
#include <arch/intr.h>
#include <arch/x86_64/intr_decode.h>
#include <arch/x86_64/intr_dispatch.h>

/* Exception vectors this phase migrates (spec §6.1) — keep in sync
 * with kernel/arch/x86_64/intr/intr_decode.c. */
#define X86_VECTOR_DIVIDE_ERROR        0ULL
#define X86_VECTOR_UNDEFINED_OPCODE    6ULL
#define X86_VECTOR_GENERAL_PROTECTION  13ULL
#define X86_VECTOR_PAGE_FAULT          14ULL

/* Member-by-member comparison of two events (spec §9.1: struct
 * intr_event has padding — memcmp is not usable).  Kept static: only
 * this TU re-validates; nothing else may treat it as an event equality
 * operator. */
static bool x86_event_matches(const struct intr_event *a,
                              const struct intr_event *b)
{
    return a->kind             == b->kind
        && a->origin           == b->origin
        && a->reason           == b->reason
        && a->access           == b->access
        && a->fault_addr_valid == b->fault_addr_valid
        && a->fault_addr       == b->fault_addr
        && a->native_vector    == b->native_vector
        && a->native_status    == b->native_status
        && a->native_address   == b->native_address;
}

__attribute__((no_stack_protector))
void arch_intr_handle_sync(const struct intr_event *event, pt_regs_t *regs)
{
    struct intr_event redecoded;

    /* 1. Deterministic re-validation: re-decode the pure payload from
     *    the event's own raw fields and compare member by member.  The
     *    raw fields ARE the inputs; no hardware state is touched. */
    x86_intr_decode(&redecoded, regs,
                    event->native_vector, event->native_status,
                    event->native_address);
    if (!x86_event_matches(event, &redecoded)) {
        arch_intr_bad_event(event, regs);
    }

    /* 2. Origin whitelist: CPL 1/2 frames are not a migrated entry. */
    if (event->origin == INTR_ORIGIN_UNKNOWN) {
        arch_intr_bad_event(event, regs);
    }

    /* 3. Explicit four-case whitelist on the native vector.  The
     *    ORIGINAL error code (and, for #PF, the ORIGINAL CR2 snapshot)
     *    is handed to the private business handler; the backend never
     *    calls back into the public do_* entries. */
    switch (event->native_vector) {
    case X86_VECTOR_DIVIDE_ERROR:
        x86_handle_divide_error(regs, event->native_status);
        return;
    case X86_VECTOR_UNDEFINED_OPCODE:
        x86_handle_undefined_opcode(regs, event->native_status);
        return;
    case X86_VECTOR_GENERAL_PROTECTION:
        x86_handle_general_protection(regs, event->native_status);
        return;
    case X86_VECTOR_PAGE_FAULT:
        x86_handle_page_fault(regs, event->native_status,
                              event->native_address);
        return;
    default:
        /* Unmigrated vectors fail closed — no default routing. */
        arch_intr_bad_event(event, regs);
    }
}

void arch_intr_handle_irq(const struct intr_event *event, pt_regs_t *regs)
{
    /* x86 IRQ backend is fail-closed in phase one (spec §5.2): no
     * production caller exists and it must never forward to
     * arch_irq_dispatch().  Calling it at all is a broken contract. */
    arch_intr_bad_event(event, regs);
}
