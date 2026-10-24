#ifndef _KERNEL_INTR_DISPATCH_H
#define _KERNEL_INTR_DISPATCH_H

// ─────────────────────────────────────────────────────────
//  intr/dispatch.h — normalized interrupt/exception event
//
//  Architecture-neutral contract (design spec §5.1).  One event
//  describes ONE synchronous fault or ONE IRQ, normalized out of
//  the native hardware encoding.
//
//  Lifecycle: the event is fully initialized on the entry
//  adapter's stack and is valid only for the duration of that
//  call.  It is a read-only snapshot — a backend must not store
//  the pointer, queue it, or park it in a global / per-CPU slot.
//
//  Address rule: an invalid address is expressed as
//  `fault_addr_valid = false, fault_addr = 0`.  Address 0 may
//  itself be a valid fault address, so the numeric value never
//  replaces the validity bit.  `native_address` always keeps the
//  raw CR2 / FAR value for diagnostics, valid or not.
//
//  This header defines NO ISA register, PTE bit or interrupt
//  controller type — only the normalized vocabulary below.  The
//  only architecture knowledge it carries is the type of the
//  register frame, obtained through the arch/regs.h facade.
// ─────────────────────────────────────────────────────────

#include <stdint.h>
#include <stdbool.h>

#include <arch/regs.h>

/* Synchronous fault (exception) vs. asynchronous interrupt. */
enum intr_kind {
    INTR_SYNC,
    INTR_IRQ
};

/* Privilege level the event was taken at. */
enum intr_origin {
    INTR_ORIGIN_KERNEL,
    INTR_ORIGIN_USER,
    INTR_ORIGIN_UNKNOWN
};

/* Hardware class of the fault.  It says what the CPU reported, NOT
 * that the fault is recoverable. */
enum intr_reason {
    INTR_REASON_OTHER,
    INTR_REASON_MEMORY,
    INTR_REASON_ARITHMETIC,
    INTR_REASON_ILLEGAL,
    INTR_REASON_PROTECTION
};

/* Access type that caused the fault, when the syndrome says so. */
enum intr_access {
    INTR_ACCESS_UNKNOWN,
    INTR_ACCESS_READ,
    INTR_ACCESS_WRITE,
    INTR_ACCESS_EXEC
};

struct intr_event {
    enum intr_kind   kind;
    enum intr_origin origin;
    enum intr_reason reason;
    enum intr_access access;
    bool             fault_addr_valid;
    uint64_t         fault_addr;
    uint64_t         native_vector;
    uint64_t         native_status;
    uint64_t         native_address;
};

/* Common stateless entry (spec §5.2).  Declared here; implemented by
 * the common dispatcher, which validates the arguments and the kind
 * and then routes to the architecture backend.  The early-reachable
 * call chain must not depend on the stack canary. */
__attribute__((no_stack_protector))
void arch_intr_dispatch(const struct intr_event *event, pt_regs_t *regs);

#endif /* _KERNEL_INTR_DISPATCH_H */
