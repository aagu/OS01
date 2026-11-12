// ─────────────────────────────────────────────────────────
//  arch/aarch64/intr/dispatch.c — aarch64 routing backend
//
//  Design spec §5.2 / §6.2.  The STRONG per-architecture
//  definitions the common dispatcher (kernel/intr/dispatch.c)
//  links against:
//
//    arch_intr_handle_sync() — deterministic re-validation, then
//        the ORIGINAL fatal path (aarch64_el1_sync_fatal) or
//        bad-event.  EL1h sync stays a kernel-fatal semantic in
//        phase one: nothing here recovers.
//    arch_intr_handle_irq()  — validation, then the existing GIC
//        dispatcher, exactly once.  IAR/EOI ownership stays with
//        the GIC backend; this file never ACKs, EOIs or touches
//        IRQ state.
//
//  Deterministic re-validation (plan "重解码纯载荷并逐成员比较"):
//  each backend re-decodes the pure payload from the event's own
//  raw fields (native_status = raw ESR, native_address = raw FAR /
//  the IRQ event's saved frame) and compares the result MEMBER BY
//  MEMBER with the event it was handed.  struct intr_event has
//  padding, so memcmp is never used.  Any disagreement means the
//  contract is broken → arch_intr_bad_event().
//
//  Normal images accept EL1h frames only. The M4 probe also admits
//  EL0t IRQs with an active private root, and cross-checks the actual
//  VBAR slot against the saved mode. EL1 sync policy is unchanged.
//
//  Sync contradiction rule: ESR.EC 0x20/0x24 (instruction/data
//  abort taken from a LOWER EL) cannot occur in a frame whose
//  saved SPSR says EL1h — that is a broken contract (bad-event).
//  Every other unknown EC is routed to the ORIGINAL fatal path;
//  the backend does not pretend to recover and invents no new
//  diagnostic.
//
//  Early-reachable chain: arch_intr_handle_sync is reached from
//  the EL1h sync entry before any boot state exists, so it is
//  no_stack_protector and touches no lock, allocation, ordinary
//  logger or per-CPU state.  arch_intr_handle_irq is a normal
//  runtime path and keeps its ordinary runtime contract.
// ─────────────────────────────────────────────────────────

#include <stdbool.h>

#include <arch/regs.h>
#include <arch/intr.h>
#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/intr_dispatch.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/user_task.h>
#if AARCH64_EL0_TEST
#include <arch/aarch64/el0.h>
#endif

/* ESR_EL1.EC values that describe a fault taken from a LOWER EL —
 * impossible in a frame saved by the EL1h sync entry. */
#define A64_EC_INSTRUCTION_ABORT_LOWER 0x20ULL
#define A64_EC_DATA_ABORT_LOWER        0x24ULL

/* SPSR_EL1 mode field M[4:0] and the only mode phase one accepts. */
#define A64_SPSR_MODE_MASK             0x1FULL
#define A64_SPSR_MODE_EL1H             0x05ULL

/* Member-by-member comparison of two events (spec §9.1: struct
 * intr_event has padding — memcmp is not usable).  Kept static: only
 * this TU re-validates; nothing else may treat it as an event equality
 * operator. */
static bool aarch64_event_matches(const struct intr_event *a,
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

/* The saved frame must say EL1h and the event must agree. */
static bool aarch64_frame_is_el1h(const struct intr_event *event,
                                  const pt_regs_t *regs)
{
    return (regs->spsr_el1 & A64_SPSR_MODE_MASK) == A64_SPSR_MODE_EL1H
        && event->origin == INTR_ORIGIN_KERNEL;
}

__attribute__((no_stack_protector))
void arch_intr_handle_sync(const struct intr_event *event, pt_regs_t *regs)
{
    struct intr_event redecoded;
    uint64_t esr = event->native_status;    /* raw ESR snapshot   */
    uint64_t far = event->native_address;   /* raw FAR snapshot   */
    uint64_t ec;

    /* 1. Deterministic re-validation: re-decode the pure payload from the
     *    event's own raw fields and compare member by member. */
    aarch64_intr_decode_sync(&redecoded, regs, esr, far);
    if (!aarch64_event_matches(event, &redecoded)) {
        arch_intr_bad_event(event, regs);
    }

    /* 2. Origin whitelist: only EL1h frames belong to this entry. */
    if (!aarch64_frame_is_el1h(event, regs)) {
        arch_intr_bad_event(event, regs);
    }

    /* 3. A lower-EL abort EC contradicts a saved EL1h frame. */
    ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
    if (ec == A64_EC_INSTRUCTION_ABORT_LOWER || ec == A64_EC_DATA_ABORT_LOWER) {
        arch_intr_bad_event(event, regs);
    }

    /* 4. Everything else keeps the original kernel-fatal semantic: the
     *    ORIGINAL raw ESR/FAR go to the frozen diagnostic (its own FnV
     *    rule derives `far=n/a` from the raw ESR).  Never returns. */
    aarch64_el1_sync_fatal(regs, esr, far);
}

/* Admission of a lower-EL IRQ frame.
 *
 * Two producers exist for an EL0t frame:
 *   - the legacy M4 probe, which owns a private root while it runs;
 *   - a real EL0 user task, whose root is the one actually installed
 *     in TTBR0_EL1.
 *
 * The formal branch reuses the CPU, the scheduler-readiness, the
 * current-user-task, the EL0t and the actual-TTBR0 checks.  A frame
 * that matches neither producer is an unrecognised source and is
 * rejected. */
static bool aarch64_el0_irq_frame_is_valid(const struct intr_event *event,
                                            const pt_regs_t *regs)
{
    if (event->origin != INTR_ORIGIN_USER)
        return false;
    if ((regs->spsr_el1 & A64_SPSR_MODE_MASK) != 0)
        return false;
#if AARCH64_EL0_TEST
    /* The legacy probe owns the root while it runs. */
    if (aarch64_el0_active_root())
        return true;
#endif
    return aarch64_user_frame_is_valid(regs);
}

void arch_intr_handle_irq(const struct intr_event *event, pt_regs_t *regs)
{
    struct intr_event redecoded;

    /* 1. Deterministic re-validation against the pure IRQ decode of the
     *    saved frame (zero native payload, no fault fields). */
    aarch64_intr_decode_irq(&redecoded, regs);
    if (!aarch64_event_matches(event, &redecoded)) {
        arch_intr_bad_event(event, regs);
    }

    bool valid = aarch64_frame_is_el1h(event, regs);
    valid |= aarch64_el0_irq_frame_is_valid(event, regs);
    if (!valid)
        arch_intr_bad_event(event, regs);

    /* 3. The existing GIC dispatcher owns IAR/EOI and the handler table
     *    (including SGI shootdown and spurious INTID handling); it is
     *    called EXACTLY once and the IRQ path returns normally. */
    gic_dev_dispatch(gic_dev_current(), regs);
}
