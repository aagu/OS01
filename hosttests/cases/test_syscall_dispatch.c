#include <errno.h>
#include <stdio.h>
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

int main(void)
{
    syscall_ctx_t ctx = { .nr = UINT64_MAX };
    if (syscall_has_handler(SYS_getpeername) || syscall_has_handler(62) ||
        syscall_has_handler(UINT64_MAX))
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
        SYS_brk, SYS_mmap, SYS_mprotect, SYS_munmap, SYS_futex,
        SYS_time, SYS_gettimeofday, SYS_clock_gettime, SYS_nanosleep, SYS_times,
        SYS_putchar, SYS_getrandom, SYS_sync, SYS_reboot, SYS_uname,
        SYS_socket, SYS_connect, SYS_sendto, SYS_recvfrom, SYS_bind,
        SYS_listen, SYS_accept, SYS_setsockopt, SYS_getsockname,
        SYS_getifaddr, SYS_getsockopt, SYS_shutdown,
    };
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        ctx = (syscall_ctx_t){ .nr = calls[i],
            .args = { 11, 22, 33, 44, 55, 66 }, .arch_frame = &ctx };
        received = NULL;
        if (!syscall_has_handler(ctx.nr) || syscall_dispatch(&ctx) != -EBADF ||
            received != &ctx || ctx.arch_frame != &ctx || ctx.suppress_writeback) {
            fprintf(stderr, "syscall %llu did not preserve dispatch context\n",
                    (unsigned long long)ctx.nr);
            return 1;
        }
        for (unsigned j = 0; j < 6; j++)
            if (ctx.args[j] != 11 * (j + 1)) return 1;
    }
    puts("syscall dispatch: PASS");
    return 0;
}
