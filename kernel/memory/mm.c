// kernel/memory/mm.c — shared mm_t lifecycle (dual-arch compiled).
// Extracted verbatim from kernel/memory/vma.c (aarch64 M4 user-mm plan
// Task 1): vma.c stays x86-only, while the aarch64 whitelist compiles
// this file so both architectures share the single mm_alloc definition.
#include <memory/mm.h>
#include <sched/task.h>   // complete mm_t (list_t vma_list, spinlock_T lock)
#include <stdlib.h>                 // calloc

// mm_alloc — allocate + initialize an mm_t.  Centralizes the "lock = 1L"
// invariant so no call site can forget it (lock == 0 is the LOCKED state
// and would deadlock the first taker).
mm_t *mm_alloc(void)
{
    mm_t *mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (!mm) return NULL;
    list_init(&mm->vma_list);
    mm->mmap_base = 0x40000000;
    spin_init(&mm->lock);
    return mm;
}
