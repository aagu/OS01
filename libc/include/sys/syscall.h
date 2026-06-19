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

static inline int64_t exec(const char *path)
{
    return syscall(SYS_exec, (uint64_t)path, 0, 0);
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
