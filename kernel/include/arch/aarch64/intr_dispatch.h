#ifndef _KERNEL_ARCH_AARCH64_INTR_DISPATCH_H
#define _KERNEL_ARCH_AARCH64_INTR_DISPATCH_H

// ─────────────────────────────────────────────────────────
//  arch/aarch64/intr_dispatch.h — private aarch64 dispatch
//  interface (design spec §5.2 / §6.2)
//
//  This header is the private interface of the aarch64 routing
//  backend (kernel/arch/aarch64/intr/dispatch.c) and the EL1h
//  entry adapters (kernel/arch/aarch64/intr/trap.c).  It is
//  NOT public ABI: the common dispatcher (kernel/intr/dispatch.c)
//  must not include it, and it must not include any private
//  header of the other architecture.
//
//  It declares the cross-TU calls the backend needs from trap.c
//  (the original fatal diagnostic) and the entry adapter that
//  entry.S's el1_sync_entry calls.  Both sit on the early-
//  reachable chain and therefore must not depend on the stack
//  canary.
// ─────────────────────────────────────────────────────────

#include <stdint.h>

#include <arch/regs.h>

/* entry.S el1_sync_entry's C landing point (slot 0x200).  Builds the
 * normalized event from the raw ESR/FAR snapshot, enters the common
 * dispatcher and never comes back: an unexpected return of the common
 * entry goes straight to arch_intr_bad_event(). */
__attribute__((noreturn, no_stack_protector))
void aarch64_el1_sync(pt_regs_t *regs, uint64_t esr, uint64_t far);

/* The pre-existing EL1h sync fatal diagnostic (frozen output contract,
 * spec §8): masks DAIF, claims the first-report flag, prints the fixed
 * `[aarch64-sync] FATAL ...` line and parks the CPU forever.  The
 * backend hands it the ORIGINAL raw ESR/FAR so its own FnV-derived
 * `far=n/a` rule keeps working. */
__attribute__((noreturn, no_stack_protector, cold))
void aarch64_el1_sync_fatal(struct pt_regs *regs, uint64_t esr, uint64_t far);

#endif /* _KERNEL_ARCH_AARCH64_INTR_DISPATCH_H */
