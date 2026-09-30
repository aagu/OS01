#ifndef _KERNEL_ARCH_AARCH64_SYNC_FAULT_H
#define _KERNEL_ARCH_AARCH64_SYNC_FAULT_H

// ─────────────────────────────────────────────────────────
//  aarch64 EL1h sync-fault helper — pure ESR/FnV decoding
//
//  Spec §4: only the following ESR_EL1.EC values carry a meaningful
//  FAR_EL1 and only when ISS.FnV (bit 10) is clear:
//
//    0x20  Instruction abort, lower EL
//    0x21  Instruction abort, same EL
//    0x22  PC alignment fault            (no FnV bit — always valid)
//    0x24  Data abort, lower EL
//    0x25  Data abort, same EL
//    0x34  Watchpoint, lower EL (or same EL, FF1==0)
//    0x35  Watchpoint, same EL
//
//  All other EC values (BRK, SVC, FP, …) must report `far=n/a`.
//
//  This is a pure helper — no globals, no MMIO, no IRQs. Used by
//  trap.c::aarch64_el1_sync_fatal and host-tested against the same
//  truth table.
// ─────────────────────────────────────────────────────────

#include <stdint.h>
#include <stdbool.h>

/* ESR_EL1.EC occupies bits [31:26]. */
#define AARCH64_ESR_EC_SHIFT  26
#define AARCH64_ESR_EC_MASK   (0x3FUL << AARCH64_ESR_EC_SHIFT)

/* ISS.FnV — bit 10 — "FAR not Valid" for instruction aborts, data
 * aborts, and watchpoints.  PC alignment (0x22) does NOT define this
 * bit, so callers must not apply FnV to it. */
#define AARCH64_ESR_FNV_BIT   (1UL << 10)

static inline bool aarch64_sync_far_valid(uint64_t esr)
{
    unsigned ec = (unsigned)((esr & AARCH64_ESR_EC_MASK)
                             >> AARCH64_ESR_EC_SHIFT);
    bool fnv = (esr & AARCH64_ESR_FNV_BIT) != 0;
    switch (ec) {
    case 0x20: case 0x21:  /* instruction aborts */
    case 0x24: case 0x25:  /* data aborts */
    case 0x34: case 0x35:  /* watchpoints */
        return !fnv;
    case 0x22:             /* PC alignment: no FnV — always valid */
        return true;
    default:
        return false;
    }
}

#endif /* _KERNEL_ARCH_AARCH64_SYNC_FAULT_H */
