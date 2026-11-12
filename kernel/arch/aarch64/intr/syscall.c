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
#include <errno.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>

void aarch64_user_syscall(pt_regs_t *regs)
{
    /* The full x8 is the number: a hole, a defined-but-unserved entry
     * or an out-of-range value has no handler and reports -ENOSYS. */
    uint64_t nr = regs->x8;
    if (!syscall_has_handler(nr)) {
        regs->x0 = (uint64_t)-ENOSYS;
        return;
    }

    syscall_ctx_t ctx = {
        .nr = nr,
        .args = {
            regs->x0, regs->x1, regs->x2,
            regs->x3, regs->x4, regs->x5,
        },
        .arch_frame = regs,
        .suppress_writeback = false,
    };

    int64_t ret = syscall_dispatch(&ctx);

    /* Both success and error keep every other register, SP, NZCV and
     * the hardware-saved return PC; only x0 carries the result. */
    if (!ctx.suppress_writeback)
        regs->x0 = (uint64_t)ret;
}
