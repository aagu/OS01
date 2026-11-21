#ifndef OS01_AARCH64_SYSCALL_H
#define OS01_AARCH64_SYSCALL_H

/* Formal AArch64 EL0 syscall entry.
 *
 * A formal user task enters the kernel with `svc #0` from EL0.  The
 * syscall NUMBER is the complete x8 and the six arguments are x0-x5;
 * the signed 64-bit result is written back to x0.  Only the SVC #0
 * immediate is a native syscall; the internal #0x4d40/#0x4d41/#0x4d42
 * immediates keep their M4 semantics in aarch64_user_task_sync().
 *
 * The hardware already saved the return PC after the SVC, so the entry
 * never adjusts ELR: every other GPR, SP, NZCV and the saved PC are
 * preserved for the ERET.
 */

#include <stddef.h>

#include <arch/aarch64/fp.h>
#include <arch/aarch64/regs.h>

/* Dispatch `regs->x8` through the shared syscall table with the live
 * exception frame as the context.  A number with no handler (a hole, a
 * defined-but-not-served entry or an out-of-range value) writes
 * -ENOSYS to x0 without dispatching.  Called from the formal
 * aarch64_user_task_sync() SVC path only. */
void aarch64_user_syscall(pt_regs_t *regs);

/* User-stack signal trampoline frame pushed before entering sa_handler
 * and consumed by SYS_sigreturn (arch_syscall_sigreturn).  The FP area
 * follows the fp.h save-area layout (q0..q31, fpsr, fpcr); delivery
 * stashes the interrupted task's live FP state in it and sigreturn
 * restores it, so an FP-using handler cannot corrupt the interrupted
 * context.  regs@0 and blocked@272 keep their historical offsets.
 *
 * The struct is 16-byte aligned on purpose: aarch64_fp_save() aligns
 * its pointer UP to 16, which is only a no-op (and therefore keeps the
 * user-visible fp area at offset 288) when the frame base itself is
 * 16-aligned.  The slab buckets are >= 32 bytes today, but the C-level
 * alignment keeps that invariant local instead of depending on
 * allocator arithmetic. */
struct __attribute__((aligned(16))) aarch64_sigframe {
    pt_regs_t regs;
    uint64_t  blocked;
    uint8_t   reserved[8];               /* keeps fp 16-byte aligned */
    uint8_t   fp[AARCH64_FP_STATE_SIZE];
};

_Static_assert(sizeof(struct aarch64_sigframe) ==
               280 + 8 + AARCH64_FP_STATE_SIZE,
               "aarch64_sigframe must be 816 bytes");
_Static_assert(offsetof(struct aarch64_sigframe, fp) == 288 &&
               (offsetof(struct aarch64_sigframe, fp) & 0xf) == 0,
               "fp area 16-byte aligned at 288");

/* Deliver pending unblocked signal(s) before returning to EL0. */
void aarch64_do_signal_delivery(pt_regs_t *regs);

#endif /* OS01_AARCH64_SYSCALL_H */
