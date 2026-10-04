#include <errno.h>
#include <stdio.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>

/* The real dispatcher routes this family to one handler.  The stand-in
 * records its input so this boundary test can catch missing registrations,
 * dropped sixth arguments, or accidental context mutation.  Behavioral FS
 * validation remains in the syscall E2E suite. */
static syscall_ctx_t *received;
int64_t sys_fs_dispatch(syscall_ctx_t *ctx)
{
    received = ctx;
    return -EBADF;
}

int main(void)
{
    syscall_ctx_t ctx = { .nr = UINT64_MAX };
    if (syscall_has_handler(SYS_getpeername) || syscall_has_handler(UINT64_MAX))
        return 1;
    if (syscall_dispatch(&ctx) != -EINVAL)
        return 1;
    const uint64_t fs_calls[] = {
        SYS_write, SYS_read, SYS_open, SYS_close, SYS_dup, SYS_dup2,
        SYS_pipe, SYS_chdir, SYS_getcwd, SYS_stat, SYS_fstat, SYS_lseek,
        SYS_fcntl, SYS_ioctl, SYS_getdents64, SYS_access, SYS_unlink,
        SYS_mkdir, SYS_rmdir, SYS_rename, SYS_truncate, SYS_ftruncate,
        SYS_chmod, SYS_fchmod, SYS_poll, SYS_ppoll, SYS_select,
        SYS_pselect6, SYS_symlink, SYS_readlink, SYS_lstat, SYS_fstatat,
    };
    for (unsigned i = 0; i < sizeof(fs_calls) / sizeof(fs_calls[0]); i++) {
        ctx = (syscall_ctx_t){ .nr = fs_calls[i],
            .args = { 11, 22, 33, 44, 55, 66 }, .arch_frame = &ctx };
        received = NULL;
        if (!syscall_has_handler(ctx.nr) || syscall_dispatch(&ctx) != -EBADF ||
            received != &ctx || ctx.arch_frame != &ctx || ctx.suppress_writeback) {
            fprintf(stderr, "FS syscall %llu did not preserve dispatch context\n",
                    (unsigned long long)ctx.nr);
            return 1;
        }
        for (unsigned j = 0; j < 6; j++)
            if (ctx.args[j] != 11 * (j + 1)) return 1;
    }
    puts("syscall dispatch: PASS");
    return 0;
}
