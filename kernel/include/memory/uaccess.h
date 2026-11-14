#ifndef _KERNEL_UACCESS_H
#define _KERNEL_UACCESS_H

// kernel/include/memory/uaccess.h — syscall-boundary DoS hardening
//
// Public surface of the uaccess subsystem: the fault-tolerant user
// copy primitives and the fast syscall-entry range check.  All
// user-pointer dereferences on the syscall / signal-delivery path MUST
// go through the primitives declared here (or through the owning
// architecture's write-preparation API — x86:
// <arch/x86_64/uaccess.h>).  See docs/superpowers/specs/2026-08-23-
// syscall-boundary-audit-design.md for the full design rationale.
//
// (mm-architecture-boundaries Task 3: the x86 address window, jmp
// buffer, strnlen_user and the locked write-preparation pair moved to
// <arch/x86_64/uaccess.h>; this header carries no MMU or VFS include
// any more.)

#include <stdint.h>
#include <stddef.h>      // size_t
#include <sys/types.h>   // ssize_t (libc + sysroot both typedef long)
#include <stdbool.h>

// Forward declarations (avoid circular task.h ↔ vma.h)
struct mm_struct;
typedef struct mm_struct mm_t;

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

// ── Fault-tolerant user-memory primitives ──────────────────
//
// All four return ssize_t for consistency with the size_t n argument:
//   >= 0: bytes copied (== n on success; never short-counted)
//   <  0: -EFAULT on fault; kernel-to-user may also return -ENOMEM during
//         COW preparation (the _ft primitives never return partial counts)
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
//   4. the owning architecture's range-accessible walker
//      (x86: arch/x86_64/mmu.h)
//
// Writable prechecks accept COW only when its VMA permits writing and is
// not VMA_IO. They do not allocate or change PTEs; actual output must use
// copy_to_user_ft or the owning architecture's locked write pair.
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);

#endif // _KERNEL_UACCESS_H
