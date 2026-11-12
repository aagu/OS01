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

/* ── The syndrome layout the decoder and the copy fixup share ──────
 *
 * One definition for both: the M4 copy fixup and the normalized event
 * decoder must agree bit for bit, otherwise a fault the fixup would
 * recover could be reported as a plain read/write (or the other way
 * round).  The layout is the ARM ARM DDI 0487 ESR_EL1.ISS encoding of
 * a data abort:
 *
 *   ISS[5:0]  DFSC  — the fault status code
 *   ISS[6]    WnR   — 1 = write, 0 = read
 *   ISS[7]    S1PTW — the fault happened inside a stage-1 page-table walk
 *   ISS[8]    CM    — cache maintenance
 *   ISS[9]    RES0
 *   ISS[10]   FnV   — the FAR is not valid
 *
 * S1PTW and CM both mean "the syndrome does not establish a read or a
 * write", so either of them suppresses the access classification.  ISS[9]
 * is reserved: on its own it changes nothing.
 */
#define AARCH64_ESR_ISS_FSC_MASK  0x3FULL
#define AARCH64_ESR_ISS_WNR       (1ULL << 6)
#define AARCH64_ESR_ISS_S1PTW     (1ULL << 7)
#define AARCH64_ESR_ISS_CM        (1ULL << 8)

/* ISS.FnV — bit 10 — "FAR not Valid" for instruction aborts, data
 * aborts, and watchpoints.  PC alignment (0x22) does NOT define this
 * bit, so callers must not apply FnV to it. */
#define AARCH64_ESR_FNV_BIT   (1UL << 10)

/* The data-abort FSC range whose syndromes may claim a read or a
 * write: translation fault 0x04..0x07, access flag fault 0x08..0x0b
 * and permission fault 0x0c..0x0f. */
#define AARCH64_FSC_ACCESS_FIRST  0x04ULL
#define AARCH64_FSC_ACCESS_LAST   0x0FULL

/* The ESR_EL1.EC values the aarch64 paths distinguish.  The "lower EL"
 * and "same EL" flavours of an abort are the SAME event seen from two
 * different privilege levels: an EL1h frame can never carry a
 * lower-EL abort, and an EL0t frame can never carry a same-EL abort. */
#define AARCH64_EC_SVC_LOWER          0x15ULL
#define AARCH64_EC_INSN_ABORT_LOWER   0x20ULL
#define AARCH64_EC_INSN_ABORT_SAME    0x21ULL
#define AARCH64_EC_PC_ALIGNMENT       0x22ULL
#define AARCH64_EC_DATA_ABORT_LOWER   0x24ULL
#define AARCH64_EC_DATA_ABORT_SAME    0x25ULL

/* SPSR_EL1 mode field M[4:0]. */
#define AARCH64_SPSR_MODE_MASK   0x1FULL
#define AARCH64_SPSR_MODE_EL0T   0x00ULL
#define AARCH64_SPSR_MODE_EL1H   0x05ULL

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
