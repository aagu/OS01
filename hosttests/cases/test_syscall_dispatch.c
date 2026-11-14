#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>

/* Family stand-ins record the production dispatcher input, catching missing
 * registrations, dropped sixth arguments, or accidental context mutation.
 * Handler behavior remains covered by the syscall E2E suite. */
static syscall_ctx_t *received;
int64_t sys_fs_dispatch(syscall_ctx_t *ctx)
{
    received = ctx;
    return -EBADF;
}

int64_t sys_mm_dispatch(syscall_ctx_t *ctx) { received = ctx; return -EBADF; }
int64_t sys_time_dispatch(syscall_ctx_t *ctx) { received = ctx; return -EBADF; }
int64_t sys_misc_dispatch(syscall_ctx_t *ctx) { received = ctx; return -EBADF; }
int64_t sys_net_dispatch(syscall_ctx_t *ctx) { received = ctx; return -EBADF; }
int64_t sys_proc_dispatch(syscall_ctx_t *ctx) { received = ctx; return -EBADF; }
int64_t sys_resource_dispatch(syscall_ctx_t *ctx) { received = ctx; return -EBADF; }

/* The four shared native entries live in kernel/syscall/sys_native.c.  Their
 * stand-ins are distinguishable from each other AND from the family stubs, so
 * a table entry wired to the wrong handler cannot hide behind a sibling's
 * value. */
#define NATIVE_EXIT_MARK          (-1001)
#define NATIVE_FORK_MARK          (-1005)
#define NATIVE_WAITPID_MARK       (-1006)
#define NATIVE_GETPID_MARK        (-1002)
#define NATIVE_GETPPID_MARK       (-1003)
#define NATIVE_CLOCK_GETTIME_MARK (-1004)

int64_t sys_native_exit(syscall_ctx_t *ctx)
{
    received = ctx;
    return NATIVE_EXIT_MARK;
}

int64_t sys_native_fork(syscall_ctx_t *ctx)
{
    received = ctx;
    return NATIVE_FORK_MARK;
}

int64_t sys_native_waitpid(syscall_ctx_t *ctx)
{
    received = ctx;
    return NATIVE_WAITPID_MARK;
}

int64_t sys_native_getpid(syscall_ctx_t *ctx)
{
    received = ctx;
    return NATIVE_GETPID_MARK;
}

int64_t sys_native_getppid(syscall_ctx_t *ctx)
{
    received = ctx;
    return NATIVE_GETPPID_MARK;
}

int64_t sys_native_clock_gettime(syscall_ctx_t *ctx)
{
    received = ctx;
    return NATIVE_CLOCK_GETTIME_MARK;
}

/* The value the shared native handler must report for `nr`; every other
 * entry belongs to a family stub. */
static int64_t expected_return(uint64_t nr)
{
    switch (nr) {
    case SYS_exit:          return NATIVE_EXIT_MARK;
    case SYS_fork:          return NATIVE_FORK_MARK;
    case SYS_waitpid:       return NATIVE_WAITPID_MARK;
    case SYS_getpid:        return NATIVE_GETPID_MARK;
    case SYS_getppid:       return NATIVE_GETPPID_MARK;
    case SYS_clock_gettime: return NATIVE_CLOCK_GETTIME_MARK;
    default:                return -EBADF;
    }
}

int main(void)
{
    syscall_ctx_t ctx = { .nr = UINT64_MAX };

    /* An empty context never reaches a handler. */
    if (syscall_dispatch(NULL) != -EINVAL)
        return 1;

#ifdef OS01_SYSCALL_A64_SUBSET
    /* ── AArch64 subset ─────────────────────────────────────────
     * The AArch64 port compiles the SAME table with the five shared
     * native entries (including fork, the one process number M4
     * opens) plus the four anonymous-VM numbers (brk/mmap/mprotect/
     * munmap).  Every other number must be absent: a leftover entry
     * would let an unsupported number reach user mode through the
     * AArch64 SVC path. */
    static const uint64_t shared[] = {
        SYS_exit, SYS_getpid, SYS_getppid, SYS_clock_gettime, SYS_fork,
        SYS_waitpid, SYS_brk, SYS_mmap, SYS_mprotect, SYS_munmap,
    };
    for (unsigned i = 0; i < sizeof(shared) / sizeof(shared[0]); i++) {
        if (!syscall_has_handler(shared[i])) {
            fprintf(stderr, "shared syscall %llu must have a handler\n",
                    (unsigned long long)shared[i]);
            return 1;
        }
        if (!syscall_name(shared[i])) {
            fprintf(stderr, "shared syscall %llu must have a trace name\n",
                    (unsigned long long)shared[i]);
            return 1;
        }
        ctx = (syscall_ctx_t){ .nr = shared[i],
            .args = { 11, 22, 33, 44, 55, 66 }, .arch_frame = &ctx };
        received = NULL;
        int64_t ret = syscall_dispatch(&ctx);
        if (ret != expected_return(shared[i]) || received != &ctx ||
            ctx.arch_frame != &ctx || ctx.suppress_writeback) {
            fprintf(stderr, "shared syscall %llu did not reach its own handler\n",
                    (unsigned long long)shared[i]);
            return 1;
        }
        for (unsigned j = 0; j < 6; j++)
            if (ctx.args[j] != 11 * (j + 1)) return 1;
    }

    /* Numbers the AArch64 port does not serve: a plain read, the
     * unimplemented getpeername slot, futex (x86-only block), an
     * out-of-range number, and a high-bit polluted number. */
    if (syscall_has_handler(SYS_read) || syscall_has_handler(SYS_getpeername) ||
        syscall_has_handler(SYS_futex) ||
        syscall_has_handler(UINT64_MAX) ||
        syscall_has_handler((1ULL << 32) | SYS_getpid)) {
        fprintf(stderr, "AArch64 subset exposes an unsupported syscall\n");
        return 1;
    }
    const uint64_t hidden[] = {
        SYS_read, SYS_getpeername, SYS_futex, UINT64_MAX,
        (1ULL << 32) | SYS_getpid,
    };
    for (unsigned i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++) {
        ctx.nr = hidden[i];
        if (syscall_dispatch(&ctx) != -EINVAL) {
            fprintf(stderr, "hidden syscall %llu must dispatch to -EINVAL\n",
                    (unsigned long long)hidden[i]);
            return 1;
        }
    }
#else
    /* ── Full x86 table ─────────────────────────────────────────
     * The x86 port keeps the complete set: every registered number
     * still reaches a handler and the context survives untouched. */
    if (syscall_has_handler(SYS_getpeername) || syscall_has_handler(62) ||
        syscall_has_handler(UINT64_MAX) ||
        !syscall_name(SYS_getpeername) ||
        strcmp(syscall_name(SYS_getpeername), "getpeername") != 0)
        return 1;
    ctx.nr = 62;
    if (syscall_dispatch(&ctx) != -EINVAL)
        return 1;
    ctx.nr = UINT64_MAX;
    if (syscall_dispatch(&ctx) != -EINVAL)
        return 1;
    const uint64_t calls[] = {
        SYS_write, SYS_read, SYS_open, SYS_close, SYS_dup, SYS_dup2,
        SYS_pipe, SYS_chdir, SYS_getcwd, SYS_stat, SYS_fstat, SYS_lseek,
        SYS_fcntl, SYS_ioctl, SYS_getdents64, SYS_access, SYS_unlink,
        SYS_mkdir, SYS_rmdir, SYS_rename, SYS_truncate, SYS_ftruncate,
        SYS_chmod, SYS_fchmod, SYS_poll, SYS_ppoll, SYS_select,
        SYS_pselect6, SYS_symlink, SYS_readlink, SYS_lstat, SYS_fstatat,
        SYS_getrlimit, SYS_setrlimit,
        SYS_brk, SYS_mmap, SYS_mprotect, SYS_munmap, SYS_futex,
        SYS_time, SYS_gettimeofday, SYS_clock_gettime, SYS_nanosleep, SYS_times,
        SYS_putchar, SYS_getrandom, SYS_sync, SYS_reboot, SYS_uname,
        SYS_socket, SYS_connect, SYS_sendto, SYS_recvfrom, SYS_bind,
        SYS_listen, SYS_accept, SYS_setsockopt, SYS_getsockname,
        SYS_getifaddr, SYS_getsockopt, SYS_shutdown,
        SYS_exit, SYS_getpid, SYS_exec, SYS_fork, SYS_waitpid, SYS_getppid,
        SYS_umask, SYS_kill, SYS_signal, SYS_sigprocmask, SYS_sigreturn,
        SYS_setpgid, SYS_getpgid, SYS_setsid, SYS_getsid,
    };
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        ctx = (syscall_ctx_t){ .nr = calls[i],
            .args = { 11, 22, 33, 44, 55, 66 }, .arch_frame = &ctx };
        received = NULL;
        if (!syscall_has_handler(ctx.nr) ||
            syscall_dispatch(&ctx) != expected_return(ctx.nr) ||
            received != &ctx || ctx.arch_frame != &ctx || ctx.suppress_writeback) {
            fprintf(stderr, "syscall %llu did not preserve dispatch context\n",
                    (unsigned long long)ctx.nr);
            return 1;
        }
        for (unsigned j = 0; j < 6; j++)
            if (ctx.args[j] != 11 * (j + 1)) return 1;
    }
#endif
    puts("syscall dispatch: PASS");
    return 0;
}
