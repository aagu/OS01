#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#include <stdint.h>

// ── Syscall numbers ───────────────────────────────────────
#define SYS_putchar  0
#define SYS_write    1    // write(fd, buf, len)  — fd-based
#define SYS_exit     2
#define SYS_brk      3
#define SYS_getpid   4
#define SYS_exec     5
#define SYS_read     6    // read(fd, buf, len)   — fd-based
#define SYS_open     7
#define SYS_close    8
#define SYS_dup      9
#define SYS_dup2    10
#define SYS_fork    11
#define SYS_waitpid 12
#define SYS_pipe    13
#define SYS_chdir   14
#define SYS_getcwd  15
#define SYS_stat    16
#define SYS_fstat   17
#define SYS_lseek   18
#define SYS_fcntl   19
#define SYS_ioctl   20
#define SYS_getdents64 21
#define SYS_access  22
#define SYS_unlink  23
#define SYS_mkdir   24
#define SYS_rmdir   25
#define SYS_rename  26
#define SYS_truncate 27
#define SYS_ftruncate 28
#define SYS_time        29
#define SYS_gettimeofday 30
#define SYS_nanosleep   31
#define SYS_chmod       32
#define SYS_fchmod      33
#define SYS_times       34
#define SYS_uname       35
#define SYS_getppid     36
#define SYS_umask       37
#define SYS_kill        38
#define SYS_signal      39

// ── Generic syscall helper ─────────────────────────────────

static inline int64_t syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    int64_t ret;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3)
        : "memory");
    return ret;
}

// ── Convenience wrappers (inline for hot paths) ────────────

static inline int64_t exec(const char *path, char *const argv[], char *const envp[])
{
    return syscall(SYS_exec, (uint64_t)path, (uint64_t)argv, (uint64_t)envp);
}

static inline int64_t fork(void)
{
    return syscall(SYS_fork, 0, 0, 0);
}

static inline void exit(int code)
{
    syscall(SYS_exit, (uint64_t)code, 0, 0);
    __builtin_unreachable();
}

#endif
