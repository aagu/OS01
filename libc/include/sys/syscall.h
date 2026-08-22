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
#define SYS_sync        40
#define SYS_reboot      41
#define SYS_sigprocmask 42
#define SYS_sigreturn  43
#define SYS_mmap        44
#define SYS_mprotect    45
#define SYS_munmap      46
#define SYS_futex      47
#define SYS_poll       48
#define SYS_ppoll      49   // v1: stub (returns -ENOSYS)
#define SYS_select     50   // v1: adaptor on top of do_poll
#define SYS_pselect6   51   // v1: adaptor on top of do_poll

// ── Socket networking (Phase 10) ───────────────────────────
#define SYS_socket        52
#define SYS_bind          53
#define SYS_connect       54
#define SYS_listen        55
#define SYS_accept        56
#define SYS_sendto        57
#define SYS_recvfrom      58
#define SYS_setsockopt    59
#define SYS_getsockopt    60
#define SYS_getsockname   61
#define SYS_getpeername   62
#define SYS_getifaddr     63
#define SYS_shutdown      64
#define SYS_clock_gettime 65
#define SYS_getrandom     66
#define SYS_setpgid       67
#define SYS_getpgid       68
#define SYS_setsid        69
#define SYS_getsid        70

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

static inline int sync(void)
{
    return (int)syscall(SYS_sync, 0, 0, 0);
}

#define RB_AUTOBOOT   0x01234567
#define RB_POWER_OFF  0x4321FEDC
#define RB_HALT_SYSTEM 0xCDEF0123

static inline int reboot(int cmd)
{
    return (int)syscall(SYS_reboot, (uint64_t)cmd, 0, 0);
}

// ── 6-argument syscall (for mmap) ─────────────────────────

static inline int64_t syscall6(uint64_t nr,
                                uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8  __asm__("r8")  = arg5;
    register uint64_t r9  __asm__("r9")  = arg6;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3),
          "r" (r10), "r" (r8), "r" (r9)
        : "memory");
    return ret;
}

#endif
