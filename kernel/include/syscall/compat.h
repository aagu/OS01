#ifndef _SYSCALL_COMPAT_H
#define _SYSCALL_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <syscall/dispatch.h>

/* Compatibility table entry type: int16_t prevents overflow past 127 */
typedef int16_t compat_syscall_nr_t;

/* Mapping sentinels */
#define COMPAT_UNMAPPED      ((compat_syscall_nr_t) 0)
#define COMPAT_UNSUPPORTED   ((compat_syscall_nr_t)-1)

/* Linux x86_64 syscall number bound (currently up to 318 getrandom) */
#define LINUX_X86_64_NR_MAX  384

/**
 * Check if the given Linux syscall number is supported.
 */
bool compat_linux_has_syscall(uint64_t linux_nr);

/**
 * Look up the OS01 native syscall number for a given Linux syscall number.
 * Returns native SYS_xxx, COMPAT_UNMAPPED, or COMPAT_UNSUPPORTED.
 */
compat_syscall_nr_t compat_linux_lookup_nr(uint64_t linux_nr);

/**
 * Linux ABI syscall dispatch entry.
 *
 * Rules:
 * 1. If linux_nr maps to a native syscall, remap ctx->nr and call syscall_dispatch(ctx);
 * 2. If linux_nr has a domain adapter (e.g. rt_sigaction), execute adapter logic;
 * 3. If linux_nr is out of bounds, unmapped, or unsupported, return -ENOSYS (-38);
 * 4. Never dispatches to unintended native syscalls.
 */
int64_t compat_linux_dispatch(syscall_ctx_t *ctx);

#endif /* _SYSCALL_COMPAT_H */
