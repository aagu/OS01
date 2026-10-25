// ─────────────────────────────────────────────────────────
//  arch/x86_64/intr/intr_decode.c — x86_64 event decoder
//
//  Pure decoding of one x86_64 exception into a normalized
//  intr_event (spec §5.1 / §6.1).
//
//  Rules implemented here:
//    * origin comes from the SAVED CS privilege level (CPL 0 →
//      KERNEL, CPL 3 → USER, CPL 1 / CPL 2 → UNKNOWN).  The #PF
//      error code U/S bit never replaces the CS.
//    * reason follows the installed exception classifications (phase2 §5).
//    * access: only #PF carries one.  I/D = 1 → EXEC, else W/R = 1
//      → WRITE, else READ.
//    * the address: only #PF owns one.  The CR2 snapshot handed in
//      by the caller becomes the fault address; every other vector
//      has no address at all (CR2 is not read here, and a #GP
//      never borrows the #PF CR2).
//
//  No system register is read, no handler is executed, nothing is
//  allocated, no lock is taken and nothing is logged.
// ─────────────────────────────────────────────────────────

#include <arch/x86_64/intr_decode.h>

#include <arch/x86_64/intr_vectors.h>

/* #PF error code bits (Intel SDM Vol. 3A, "Page-Fault Exception
 * Error Code"): bit 0 = P, bit 1 = W/R, bit 2 = U/S, bit 4 = I/D. */
#define X86_PF_ERROR_WRITE   (1ULL << 1)
#define X86_PF_ERROR_INSTR   (1ULL << 4)

/* Origin from the saved CS selector: the low two bits are the RPL,
 * i.e. the privilege level the exception was taken at. */
__attribute__((no_stack_protector))
static enum intr_origin x86_origin_from_cs(uint64_t cs)
{
    switch (cs & 3ULL) {
    case 0:
        return INTR_ORIGIN_KERNEL;
    case 3:
        return INTR_ORIGIN_USER;
    default:
        return INTR_ORIGIN_UNKNOWN;
    }
}

__attribute__((no_stack_protector))
void x86_intr_decode(struct intr_event *out, const pt_regs_t *regs,
                     uint64_t vector, uint64_t error_code, uint64_t cr2)
{
    out->kind = INTR_SYNC;
    out->origin = x86_origin_from_cs(regs->cs);
    out->native_vector = vector;
    out->native_status = error_code;

    if (vector == X86_VECTOR_PAGE_FAULT) {
        out->reason = INTR_REASON_MEMORY;
        if (error_code & X86_PF_ERROR_INSTR) {
            out->access = INTR_ACCESS_EXEC;
        } else if (error_code & X86_PF_ERROR_WRITE) {
            out->access = INTR_ACCESS_WRITE;
        } else {
            out->access = INTR_ACCESS_READ;
        }
        out->fault_addr_valid = true;
        out->fault_addr = cr2;
        out->native_address = cr2;
        return;
    }

    switch (vector) {
    case X86_VECTOR_DIVIDE_ERROR:
    case X86_VECTOR_OVERFLOW:
    case X86_VECTOR_BOUNDS:
    case X86_VECTOR_DEV_NOT_AVAILABLE:
    case X86_VECTOR_COPROCESSOR_SEGMENT_OVERRUN:
    case X86_VECTOR_X87_FPU_ERROR:
    case X86_VECTOR_SIMD_EXCEPTION:
        out->reason = INTR_REASON_ARITHMETIC;
        break;
    case X86_VECTOR_UNDEFINED_OPCODE:
        out->reason = INTR_REASON_ILLEGAL;
        break;
    case X86_VECTOR_INVALID_TSS:
    case X86_VECTOR_SEGMENT_NOT_PRESENT:
    case X86_VECTOR_STACK_SEGMENT_FAULT:
    case X86_VECTOR_GENERAL_PROTECTION:
    case X86_VECTOR_ALIGNMENT_CHECK:
    case X86_VECTOR_VIRTUALIZATION_EXCEPTION:
        out->reason = INTR_REASON_PROTECTION;
        break;
    default:
        out->reason = INTR_REASON_OTHER;
        break;
    }

    out->access = INTR_ACCESS_UNKNOWN;
    out->fault_addr_valid = false;
    out->fault_addr = 0;
    out->native_address = 0;
}

__attribute__((no_stack_protector))
void x86_intr_decode_irq(struct intr_event *out, const pt_regs_t *regs,
                         uint64_t vector)
{
    out->kind = INTR_IRQ;
    out->origin = x86_origin_from_cs(regs->cs);
    out->reason = INTR_REASON_OTHER;
    out->access = INTR_ACCESS_UNKNOWN;
    out->fault_addr_valid = false;
    out->fault_addr = 0;
    out->native_vector = vector;
    out->native_status = 0;
    out->native_address = 0;
}
