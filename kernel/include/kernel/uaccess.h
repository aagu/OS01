#ifndef _KERNEL_UACCESS_H
#define _KERNEL_UACCESS_H

// kernel/include/kernel/uaccess.h — syscall-boundary DoS hardening
//
// Public surface of the uaccess subsystem.  All user-pointer dereferences
// on the syscall / signal-delivery path MUST go through the primitives
// declared here (or through user_write_range_begin/end for the non-blocking
// kernel→user write case).  See docs/superpowers/specs/2026-08-23-syscall-
// boundary-audit-design.md for the full design rationale.

#include <stdint.h>
#include <stddef.h>      // size_t
#include <sys/types.h>   // ssize_t (libc + sysroot both typedef long)
#include <stdbool.h>
#include <kernel/arch/mmu.h>   // arch_user_range_accessible (cross-level walker)
#include <fs/vfs.h>            // VFS_NAME_MAX

// ── User address-layout constants ──────────────────────────
// Lowest legitimate user address.  USER_CODE_ADDR = 0x400000 (task.c:1048,
// trap.c:765).  The user stack lives at 0x800000 (task.h:352) with a
// 0x600000 guard left unmapped; nothing legitimate lives below 0x400000.
// do_mmap enforces this so the invariant "nothing below 0x400000 mapped"
// holds for every user task — see kernel/memory/vma.c.
#define USER_MIN_ADDR     0x400000UL

// ── Bounce-buffer block size for Cat C syscalls (read/write/sendto/
//    recvfrom).  VFS/FS callbacks write into this kernel buffer; the user
//    pointer is only touched by copy_*_ft.  64 KiB matches the largest
//    reasonable single fs read and stays clear of the kernel stack. ──────
#define UACCESS_BOUNCE_SIZE (64 * 1024)

// ── exec argv/envp limits (Cat A' deep copy, Task 5) ───────
// MAX_ARGV:        at most this many pointers in argv[]/envp[]
// MAX_ARG_STRLEN:  per-element string length cap (incl. NUL)
// MAX_ARG_TOTAL:   combined cap across all elements in argv OR envp
// Exceeding any of these returns -E2BIG (no crash).
#define MAX_ARGV          128
#define MAX_ARG_STRLEN    4096
#define MAX_ARG_TOTAL     65536

// ── Clang __builtin_setjmp/__builtin_longjmp buffer ────────
// 8 × 8 B = 64 B.  Clang emits callee-saved (RBX/RBP/R12-R15) + RSP +
// return PC; `longjmp` value argument MUST be a compile-time constant
// (kernel hooks always use 1).  See setjmp-frame-pointer-bug.md for why
// we use the builtin instead of libc setjmp.
typedef void *os01_jmp_buf[8];

// ── Fault-tolerant user-memory primitives ──────────────────
//
// All four return ssize_t for consistency with the size_t n argument:
//   >= 0: bytes copied (== n on success; never short-counted)
//   <  0: -EFAULT on fault (the _ft primitives never return partial counts)
//
// The _res variants take an explicit cleanup callback (Task 2 wires these
// to release mm->lock / wait-queue refs that the longjmp would otherwise
// leak).  The plain variants are convenience wrappers that pass NULL for
// the callback — use them when no resource needs releasing.
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg);
ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg);

static inline ssize_t copy_to_user_ft(void *dst, const void *src, size_t n)
{
    return copy_to_user_ft_res(dst, src, n, NULL, NULL);
}

static inline ssize_t copy_from_user_ft(void *dst, const void *src, size_t n)
{
    return copy_from_user_ft_res(dst, src, n, NULL, NULL);
}

// Bounded user string length.  Returns:
//   >= 0: strlen(user_addr), bounded by max
//   <  0: -EFAULT on fault (no NUL within [user_addr, user_addr+max))
//
// max is the buffer capacity (excluding the trailing NUL); the returned
// length is the number of non-NUL bytes — i.e. caller writes a NUL at
// buf[ret] to NUL-terminate.  fault path leaves buf untouched.
int strnlen_user(const void *user_addr, size_t max);

// ── Syscall entry-point check (used by every handler with user ptrs) ──
//
// Returns true iff [addr, addr+len) is fully mapped into the current task
// with effective permissions matching `writable`.  This is the fast-reject
// gate; if it returns true the handler may proceed but should still use
// copy_*_ft for the actual access (check_user_range is a snapshot, not a
// pin — munmap can race the gap between check and use).
//
// On any failure returns false (handler maps to -EFAULT).  Implements the
// four checks from the design §3:
//   1. addr != 0 && len > 0           (len==0 → true no-op)
//   2. addr >= USER_MIN_ADDR
//   3. addr < addr_limit && len <= addr_limit - addr (overflow-safe)
//   4. arch_user_range_accessible(mm->pml4, addr, len, writable)
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);

#endif // _KERNEL_UACCESS_H