#include <errno.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <uapi/time.h>
#include <sched/task.h>
#include <memory/uaccess.h>
#include <arch/clocksource.h>
#include <arch/syscall.h>
#include <log/log.h>

/* ── Shared native syscall handlers ────────────────────────────
 *
 * These four entries are the ones the AArch64 port serves as well,
 * so they live in their own translation unit instead of inside the
 * architecture-neutral dispatch families.  The unit stays free of any
 * per-architecture register access: the syscall context carries the
 * arguments, and the architecture-specific effect of exit() is
 * delegated to <arch/syscall.h>.
 */

int64_t sys_native_exit(syscall_ctx_t *ctx)
{
    // exit(int code) — terminate the current process.
    // Encode as Linux does: the exit code lives in the high byte
    // (code << 8) so waitpid() can tell a normal exit (WIFEXITED)
    // from a signal death (low byte = signal → WIFSIGNALED).
    uint64_t status = (ctx->args[0] & 0xff) << 8;
    arch_syscall_exit(status);
    __builtin_unreachable();
}

int64_t sys_native_fork(syscall_ctx_t *ctx)
{
    // fork() → child PID in the parent, 0 in the child's saved frame.
    // The architecture owns the frame fix-up; this handler keeps the
    // historical x86 trace line and return value.
    int64_t pid = arch_syscall_fork(ctx ? ctx->arch_frame : NULL);
    log_info("fork: pid=%d returned %d\n", (int)current->pid, (int)pid);
    return pid;
}

int64_t sys_native_getpid(syscall_ctx_t *ctx)
{
    (void)ctx;
    return current->pid;
}

int64_t sys_native_getppid(syscall_ctx_t *ctx)
{
    (void)ctx;
    // getppid() → parent PID (or 0 for init)
    return current->parent ? current->parent->pid : 0;
}

int64_t sys_native_clock_gettime(syscall_ctx_t *ctx)
{
    // clock_gettime(clockid_t clk_id, struct timespec *tp)
    // OS01 has no real RTC wall clock yet (gettimeofday returns 0),
    // so both CLOCK_REALTIME and CLOCK_MONOTONIC report the same
    // monotonic clocksource time (clocksource_read_ns, ns).
    uint64_t clk_id = ctx->args[0];
    struct timespec *tp = (struct timespec *)ctx->args[1];
    if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC)
        return -EINVAL;
    if (!tp)
        return -EFAULT;
    uint64_t ns = arch_clocksource_read_ns();
    struct timespec kts = {
        .tv_sec  = ns / 1000000000ULL,
        .tv_nsec = ns % 1000000000ULL,
    };
    ssize_t r = copy_to_user_ft(tp, &kts, sizeof(kts));
    if (r < 0)
        return r;
    return 0;
}
