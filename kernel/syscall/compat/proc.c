#include <syscall/compat.h>
#include <uapi/syscall.h>
#include <errno.h>
#include "internal.h"

int64_t compat_sys_rt_sigaction(syscall_ctx_t *ctx)
{
    /* Linux rt_sigaction expects sigsetsize == sizeof(sigset_t) == 8 on x86_64 */
    if (ctx->args[3] != 8)
        return -EINVAL;

    ctx->nr = SYS_signal;
    return syscall_dispatch(ctx);
}

int64_t compat_sys_wait4(syscall_ctx_t *ctx)
{
    /* Linux wait4(pid, status, options, rusage) -> SYS_waitpid(pid, status, options) */
    ctx->nr = SYS_waitpid;
    return syscall_dispatch(ctx);
}
