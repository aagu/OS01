// ─────────────────────────────────────────────────────────
//  arch/aarch64/intr/intr_decode.c — aarch64 event decoder
//
//  Pure decoding of one aarch64 exception / interrupt into a
//  normalized intr_event (spec §5.1 / §6.2).
//
//  Rules implemented here:
//    * origin comes from the SAVED SPSR_EL1 mode M[4:0]:
//      EL1h (0b00101) → KERNEL, EL0t (0) → USER, every other
//      mode → UNKNOWN.
//    * reason follows ESR_EL1.EC: instruction / data aborts
//      (0x20 / 0x21 / 0x24 / 0x25) → MEMORY, PC alignment (0x22)
//      → PROTECTION, everything else → OTHER.
//    * access: an instruction abort is EXEC; a data abort is
//      READ / WRITE only when the syndrome proves it — the FSC
//      whitelist 0x04..0x0f (translation, access-flag and
//      permission faults) with CM = 0 and S1PTW = 0 — and WnR
//      selects write over read.  Everything else stays UNKNOWN.
//    * the address: the FAR validity is the existing truth source
//      aarch64_sync_far_valid(); when it is false the normalized
//      address is invalid and zeroed, while native_address keeps
//      the raw FAR for diagnostics.
//
//  No system register is read, no handler is executed, nothing is
//  allocated, no lock is taken and nothing is logged.
// ─────────────────────────────────────────────────────────

#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/sync_fault.h>

/* ESR_EL1.EC values (spec §6.2) and the ISS layout come from the
 * sync_fault.h truth source, so the decoder and the copy fixup share
 * one definition of the syndrome. */

/* SPSR_EL1 mode M[4:0]. */
#define A64_MODE_EL0T AARCH64_SPSR_MODE_EL0T
#define A64_MODE_EL1H AARCH64_SPSR_MODE_EL1H

/* The only data abort syndromes whose access type may be decoded
 * (spec §6.2): translation fault 0x04..0x07, access flag fault
 * 0x08..0x0b, permission fault 0x0c..0x0f. */
#define A64_FSC_WHITELIST_FIRST AARCH64_FSC_ACCESS_FIRST
#define A64_FSC_WHITELIST_LAST  AARCH64_FSC_ACCESS_LAST

static enum intr_origin aarch64_origin_from_spsr(uint64_t spsr)
{
    switch (spsr & AARCH64_SPSR_MODE_MASK) {
    case A64_MODE_EL1H:
        return INTR_ORIGIN_KERNEL;
    case A64_MODE_EL0T:
        return INTR_ORIGIN_USER;
    default:
        return INTR_ORIGIN_UNKNOWN;
    }
}

/* Access type of a data abort — conservative by construction: the
 * syndrome has to prove the access before READ / WRITE is claimed. */
static enum intr_access aarch64_data_abort_access(uint64_t esr)
{
    uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;

    if (fsc < A64_FSC_WHITELIST_FIRST || fsc > A64_FSC_WHITELIST_LAST) {
        return INTR_ACCESS_UNKNOWN;
    }
    if (esr & AARCH64_ESR_ISS_CM) {
        return INTR_ACCESS_UNKNOWN;
    }
    if (esr & AARCH64_ESR_ISS_S1PTW) {
        return INTR_ACCESS_UNKNOWN;
    }
    return (esr & AARCH64_ESR_ISS_WNR) ? INTR_ACCESS_WRITE : INTR_ACCESS_READ;
}

__attribute__((no_stack_protector))
void aarch64_intr_decode_sync(struct intr_event *out, const pt_regs_t *regs,
                              uint64_t esr, uint64_t far)
{
    uint64_t ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;

    out->kind = INTR_SYNC;
    out->origin = aarch64_origin_from_spsr(regs->spsr_el1);
    /* The native vector is the VBAR slot this decoder derives from the
     * SAVED mode: an EL0t frame was taken through the lower-EL sync
     * slot, an EL1h frame through the EL1h sync slot.  The entry
     * adapter states the slot it was really taken through, and the
     * backend re-decodes and compares — a frame that claims one slot
     * and arrives through another is rejected. */
    out->native_vector = (out->origin == INTR_ORIGIN_USER)
                             ? AARCH64_EL0_SYNC_SLOT
                             : AARCH64_EL1H_SYNC_SLOT;
    out->native_status = esr;
    /* The raw FAR is always kept — it is the diagnostic payload the
     * backend hands to the original fatal path. */
    out->native_address = far;

    switch (ec) {
    case AARCH64_EC_INSN_ABORT_LOWER:
    case AARCH64_EC_INSN_ABORT_SAME:
        out->reason = INTR_REASON_MEMORY;
        out->access = INTR_ACCESS_EXEC;
        break;
    case AARCH64_EC_DATA_ABORT_LOWER:
    case AARCH64_EC_DATA_ABORT_SAME:
        out->reason = INTR_REASON_MEMORY;
        out->access = aarch64_data_abort_access(esr);
        break;
    case AARCH64_EC_PC_ALIGNMENT:
        out->reason = INTR_REASON_PROTECTION;
        out->access = INTR_ACCESS_UNKNOWN;
        break;
    default:
        out->reason = INTR_REASON_OTHER;
        out->access = INTR_ACCESS_UNKNOWN;
        break;
    }

    /* The FAR validity follows the existing truth source; the PC
     * alignment case is not subject to FnV there. */
    if (aarch64_sync_far_valid(esr)) {
        out->fault_addr_valid = true;
        out->fault_addr = far;
    } else {
        out->fault_addr_valid = false;
        out->fault_addr = 0;
    }
}

__attribute__((no_stack_protector))
void aarch64_intr_decode_irq(struct intr_event *out, const pt_regs_t *regs)
{
    out->kind = INTR_IRQ;
    out->origin = aarch64_origin_from_spsr(regs->spsr_el1);
    out->reason = INTR_REASON_OTHER;
    out->access = INTR_ACCESS_UNKNOWN;
    out->fault_addr_valid = false;
    out->fault_addr = 0;
    out->native_vector = AARCH64_EL1H_IRQ_SLOT;
    /* A frame whose saved mode is EL0t came through the lower-EL IRQ
     * slot: report that slot so the backend can cross-check it. */
    if (out->origin == INTR_ORIGIN_USER)
        out->native_vector = AARCH64_EL0_IRQ_SLOT;
    out->native_status = 0;
    out->native_address = 0;
}
