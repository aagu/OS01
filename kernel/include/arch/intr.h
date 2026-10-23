#ifndef _KERNEL_ARCH_INTR_H
#define _KERNEL_ARCH_INTR_H

// ─────────────────────────────────────────────────────────
//  arch/intr.h — architecture backend facade for event dispatch
//
//  Declares the three STRONG per-architecture backends that the
//  common dispatcher (kernel/intr/dispatch.c) calls (spec §5.2).
//  Each architecture MUST provide a strong definition of all
//  three; a missing backend must fail the link — there is no
//  weak no-op fallback.
//
//    arch_intr_handle_sync()  — one normalized synchronous fault
//    arch_intr_handle_irq()   — one normalized asynchronous IRQ
//    arch_intr_bad_event()    — fatal contract violation
//                                (NULL event / NULL regs /
//                                 out-of-range kind)
//
//  The bad-event backend is the internal "contract is broken"
//  path: it masks interrupts on this core, prints the fixed
//  prefix "[intr-dispatch] FATAL bad-event" through the
//  lock-free early-print facade and parks the CPU forever.  It
//  MUST NOT dereference its arguments (they may be NULL), MUST
//  NOT touch current/per-CPU state, MUST NOT call the ordinary
//  logger, allocate, or take a lock.
//
//  This header is architecture-NEUTRAL: it pulls in only the
//  normalized event contract (intr/dispatch.h) and the register
//  frame type (arch/regs.h).  It must NOT include any
//  architecture-private header (arch/<arch>/intr_decode.h,
//  arch/<arch>/intr_dispatch.h) — those belong to the
//  per-architecture backends.
//
//  Include order: intr/dispatch.h includes arch/regs.h; arch/regs.h
//  must never include this header (no reverse dependency).
// ─────────────────────────────────────────────────────────

#include <arch/regs.h>
#include <intr/dispatch.h>

/* Route one normalized synchronous fault to the architecture's
 * private handler.  The common dispatcher passes the original
 * event / regs pointers through unchanged. */
void arch_intr_handle_sync(const struct intr_event *event, pt_regs_t *regs);

/* Route one normalized asynchronous interrupt to the
 * architecture's private handler. */
void arch_intr_handle_irq(const struct intr_event *event, pt_regs_t *regs);

/* Fatal contract violation: the common dispatcher could not
 * produce a usable event (NULL event, NULL regs, or an
 * out-of-range kind).  Never returns. */
__attribute__((noreturn))
void arch_intr_bad_event(const struct intr_event *event, pt_regs_t *regs);

#endif /* _KERNEL_ARCH_INTR_H */
