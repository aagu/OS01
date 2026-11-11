#ifndef _KERNEL_MEMORY_MM_H
#define _KERNEL_MEMORY_MM_H

// Forward declaration — keeps this header light (mirrors vma.h; the
// complete mm_t lives in <sched/task.h>).
struct mm_struct;
typedef struct mm_struct mm_t;

// mm_alloc — allocate + initialize an mm_t.  Centralizes the "lock = 1L"
// invariant so no call site can forget it (lock == 0 is the LOCKED state
// and would deadlock the first taker).
// Returns the new mm or NULL on allocation failure.  Dual-arch compiled
// (aarch64 M4 user-mm consumes it from kernel/arch/aarch64/memory/user_mm.c).
mm_t *mm_alloc(void);

#endif // _KERNEL_MEMORY_MM_H
