#ifndef _ARCH_X86_64_MSR_SAFE_H
#define _ARCH_X86_64_MSR_SAFE_H

/*
 * Instruction-scoped safe RDMSR (kernel/monitor thermal plan, Task 1).
 *
 * `arch_rdmsr_safe` is the safe-RDMSR primitive: it executes `rdmsr`
 * tagged with `arch_rdmsr_safe_try` so that a #GP raised by an unknown
 * or reserved MSR is caught by the #GP fixup and recovered as
 * `-EIO` without modifying the caller's `*value`.  This is the only
 * place the kernel uses `rdmsr` outside of pre-boot code paths that
 * have already proved the MSR exists.
 *
 * `arch_msr_gp_fixup` is the #GP fixup half of the contract.  It
 * returns true ONLY when the faulting RIP is the literal `rdmsr`
 * instruction (`arch_rdmsr_safe_try`) AND the trap was raised from
 * ring 0 (`(regs->cs & 3) == 0`) AND the GP error code is 0.  On
 * match it rewrites `regs->rip` to `arch_rdmsr_safe_fail` (the
 * "load -EIO into EAX and return" continuation in msr_safe.S); any
 * other field of `regs` is left untouched.  On any non-match it
 * returns false WITHOUT modifying `regs`, so the original
 * do_general_protection path (user-kill or kernel halt) continues
 * to run for unmarked faults.
 *
 * The label addresses are exported as `const char[]` so the host
 * fixture (hosttests/cases/test_thermal_fixup.c) and the in-kernel
 * selftest (kernel/selftest/test_msr_safe.c, gated on
 * KERNEL_RUN_MSRS_SAFE_SELFTEST) can both compare them by identity
 * without inspecting the actual instruction bytes.
 */

#include <stdbool.h>
#include <stdint.h>

#include <arch/x86_64/regs.h>   /* pt_regs_t */

/* Public-facing prototype — kernel build AND hosttests include this. */
int  arch_rdmsr_safe(uint32_t msr, uint64_t *value);

/* #GP fixup.  Returns true iff the faulting RIP is the marked
 * `rdmsr` AND CPL==0 AND error_code==0; on true, redirects RIP
 * to the fail label.  Any other case returns false and leaves
 * `regs` untouched.  NULL `regs` is a defensive no-op. */
bool arch_msr_gp_fixup(pt_regs_t *regs, uint64_t error_code);

/* Assembly labels — the bytes at `arch_rdmsr_safe_try` are the
 * single `rdmsr` instruction (0x0f 0x32); `arch_rdmsr_safe_fail`
 * is the next instruction (the "load -EIO, ret" continuation). */
extern const char arch_rdmsr_safe_try[];
extern const char arch_rdmsr_safe_fail[];

#endif /* _ARCH_X86_64_MSR_SAFE_H */
