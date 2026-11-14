#ifndef _ARCH_X86_64_UACCESS_H
#define _ARCH_X86_64_UACCESS_H

/*
 * x86_64-private uaccess declarations (mm-architecture-boundaries
 * Task 3).
 *
 * The x86 address-window, write-preparation and fault-fixup ownership.
 * The shared user-copy and range-check primitives stay in the public
 * <memory/uaccess.h>; everything here is x86-only.
 */

#include <stdint.h>
#include <stddef.h>
#include <arch/x86_64/user_layout.h>  // X86_USER_MIN_ADDR (user window)

// Forward declaration (avoids circular task.h ↔ vma.h)
struct mm_struct;
typedef struct mm_struct mm_t;

// ── Clang __builtin_setjmp/__builtin_longjmp buffer ────────
// 8 × 8 B = 64 B.  Clang emits callee-saved (RBX/RBP/R12-R15) + RSP +
// return PC; `longjmp` value argument MUST be a compile-time constant
// (kernel hooks always use 1).  See setjmp-frame-pointer-bug.md for why
// we use the builtin instead of libc setjmp.
typedef void *os01_jmp_buf[8];

// Bounded user string length.  Returns:
//   >= 0: strlen(user_addr), bounded by max
//   <  0: -EFAULT on fault (no NUL within [user_addr, user_addr+max))
//
// max is the buffer capacity (excluding the trailing NUL); the returned
// length is the number of non-NUL bytes — i.e. caller writes a NUL at
// buf[ret] to NUL-terminate.  fault path leaves buf untouched.
int strnlen_user(const void *user_addr, size_t max);

// Prepare kernel output to user memory. The locked form requires mm->lock
// and leaves ownership of that lock with the caller on every return. The
// self-locking form releases its lock before returning on every path.
// Writable ordinary leaves (including ELF/stack) need no VMA. COW leaves
// require a writable, non-VMA_IO VMA. All allocations precede PTE/refcount
// changes; failure leaves the entire range unchanged.
// Returns 0, -EFAULT (invalid range/permissions) or -ENOMEM (allocation).
int prepare_user_write_range(mm_t *mm, uint64_t addr, size_t len);
int prepare_user_write_range_locked(mm_t *mm, uint64_t addr, size_t len);

// Validate-and-lock a user write range before the kernel writes into it
// (getrandom / devfs random read).  Returns 0 with current->mm->pgdir_lock
// HELD on success — caller MUST call user_write_range_end().  Returns
// -EFAULT (lock NOT held) on any bad address or non-writable page.
// Kernel buffers (current->mm == NULL) are trusted and pass through
// without lock/check.
int  user_write_range_begin(uint64_t addr, size_t len);
void user_write_range_end(void);

#endif /* _ARCH_X86_64_UACCESS_H */
