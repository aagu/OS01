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

#include <arch/aarch64/regs.h>

/* Dispatch `regs->x8` through the shared syscall table with the live
 * exception frame as the context.  A number with no handler (a hole, a
 * defined-but-not-served entry or an out-of-range value) writes
 * -ENOSYS to x0 without dispatching.  Called from the formal
 * aarch64_user_task_sync() SVC path only. */
void aarch64_user_syscall(pt_regs_t *regs);

#endif /* OS01_AARCH64_SYSCALL_H */
