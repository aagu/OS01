/* kernel/arch/aarch64/intr/syscall.c
 *
 * Formal AArch64 EL0 native syscall entry.
 *
 * The shared table (kernel/syscall/dispatch.c, compiled with
 * OS01_SYSCALL_A64_SUBSET=1) carries only the entries the AArch64 port
 * serves.  This unit only marshals the exception frame into the
 * architecture-neutral syscall context; it touches no per-architecture
 * register beyond x0-x8 and never manages ELR, SP or PSTATE.
 */

#include <arch/aarch64/syscall.h>
#include <arch/aarch64/user_vm.h>
#include <errno.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#if AARCH64_EL0_TEST
#include <selftest/aarch64_user_fork_cow.h>
#include <selftest/aarch64_user_svc_uaccess.h>
#include <selftest/aarch64_user_vm.h>
#endif

void aarch64_user_syscall(pt_regs_t *regs)
{
    /* The full x8 is the number: a hole, a defined-but-unserved entry
     * or an out-of-range value has no handler and reports -ENOSYS. */
    uint64_t nr = regs->x8;
    if (!syscall_has_handler(nr)) {
        regs->x0 = (uint64_t)-ENOSYS;
        return;
    }

#if AARCH64_EL0_TEST
    /* M4 acceptance observation point: records the fixture's x19/x20
     * from the saved frame and runs the kernel copy self-test once, on
     * the fixture's first clock_gettime.  Test variant only. */
    aarch64_user_svc_uaccess_test_hook(regs, nr);
    aarch64_user_fork_cow_test_syscall_enter(regs, nr);
#endif

    syscall_ctx_t ctx = {
        .nr = nr,
        .args = {
            regs->x0, regs->x1, regs->x2,
            regs->x3, regs->x4, regs->x5,
        },
        .arch_frame = regs,
        .suppress_writeback = false,
    };

    /* A returning native dispatch may authorise a user-VM edit
     * transaction.  SYS_exit is skipped inside enter(); a number
     * without a handler never reaches this point. */
    aarch64_vm_native_enter(regs, nr);

    int64_t ret = syscall_dispatch(&ctx);

#if AARCH64_EL0_TEST
    /* Acceptance observation: mmap/brk must not have instantiated a
     * leaf, and the two clock_gettime -EFAULT targets are inspected
     * here while the user root is still installed. */
    aarch64_user_vm_test_syscall(regs, nr, ret);
    aarch64_user_fork_cow_test_syscall_ret(regs, nr, ret);
#endif

    aarch64_vm_native_leave();

    /* Both success and error keep every other register, SP, NZCV and
     * the hardware-saved return PC; only x0 carries the result. */
    if (!ctx.suppress_writeback)
        regs->x0 = (uint64_t)ret;
}
