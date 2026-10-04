#include <syscall/compat.h>
#include <errno.h>
#include <log/log.h>
#include <core/debug.h>
#include "internal.h"

bool compat_linux_has_syscall(uint64_t linux_nr)
{
    if (linux_nr >= LINUX_X86_64_NR_MAX)
        return false;
    compat_syscall_nr_t os_nr = linux_x86_64_table[linux_nr];
    return os_nr > 0;
}

compat_syscall_nr_t compat_linux_lookup_nr(uint64_t linux_nr)
{
    if (linux_nr >= LINUX_X86_64_NR_MAX)
        return COMPAT_UNMAPPED;
    return linux_x86_64_table[linux_nr];
}

int64_t compat_linux_dispatch(syscall_ctx_t *ctx)
{
    if (!ctx)
        return -EINVAL;

    uint64_t nr = ctx->nr;
    if (nr >= LINUX_X86_64_NR_MAX) {
        debug_syscall("[compat_linux] nr=%lu out of bounds -> -ENOSYS\n", nr);
        return -ENOSYS;
    }

    compat_syscall_nr_t os_nr = linux_x86_64_table[nr];
    if (os_nr == COMPAT_UNMAPPED || os_nr == COMPAT_UNSUPPORTED) {
        debug_syscall("[compat_linux] nr=%lu (%s) -> -ENOSYS\n",
                      nr, os_nr == COMPAT_UNSUPPORTED ? "unsupported" : "unmapped");
        return -ENOSYS;
    }

    /* Domain-specific adapters */
    if (nr == 13)
        return compat_sys_rt_sigaction(ctx);
    if (nr == 61)
        return compat_sys_wait4(ctx);

    /* Standard 1:1 mapped native syscall */
    ctx->nr = (uint64_t)os_nr;
    return syscall_dispatch(ctx);
}
