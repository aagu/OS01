/*
 * kernel/arch/x86_64/cpu/msr_safe.c
 *
 * #GP fixup half of the instruction-scoped safe RDMSR contract
 * (kernel/monitor thermal plan, Task 1).
 *
 * The companion msr_safe.S emits `arch_rdmsr_safe_try` at the exact
 * byte boundary of the rdmsr instruction (the only thing tagged),
 * and `arch_rdmsr_safe_fail` at the next instruction.  do_general_protection
 * calls arch_msr_gp_fixup(regs, error_code) as its FIRST action:
 *
 *   - On a match (CPL==0 AND error_code==0 AND regs->rip==&try)
 *     it rewrites regs->rip to &fail and returns true.  The
 *     unintercepted iretq then resumes the interrupted call with
 *     -EIO in %eax and the caller's *value untouched.
 *
 *   - On any non-match it returns false WITHOUT touching *regs;
 *     the existing do_general_protection path (user-kill or
 *     kernel halt) runs unchanged.
 *
 * The matching is intentionally strict: it is the entire point of
 * the task.  An unmarked #GP would otherwise be silently absorbed,
 * which is precisely the failure mode the plan rejects.
 */

#include <arch/x86_64/msr_safe.h>
#include <arch/x86_64/regs.h>       /* pt_regs_t, X86_KERNEL_CS bit 0/1 */

#include <errno.h>

bool arch_msr_gp_fixup(pt_regs_t *regs, uint64_t error_code)
{
    /* Defensive: a bogus regs=NULL must not crash the host build or
     * the kernel itself (host fixture and kernel selftest both probe). */
    if (!regs)
        return false;

    /* Match criteria, applied in this order:
     *   1. error_code == 0     — any non-zero GP code (selector, TI,
     *                             IDT) changes semantics; we never
     *                             swallow those.
     *   2. CPL == 0            — `cs & 3` is the active CPL per SDM.
     *                             A user-mode RIP can never be the
     *                             kernel's marked instruction.
     *   3. RIP == &try         — identity comparison against the
     *                             linker-defined object.  Any other
     *                             RIP keeps the original fatal path.
     */
    if (error_code != 0)
        return false;
    if ((regs->cs & 3) != 0)
        return false;
    if (regs->rip != (uint64_t)(uintptr_t)arch_rdmsr_safe_try)
        return false;

    /* Match: redirect RIP to the fail continuation in msr_safe.S.
     * No other field is touched (plan: "命中只把RIP移到error返回标签"). */
    regs->rip = (uint64_t)(uintptr_t)arch_rdmsr_safe_fail;
    return true;
}
