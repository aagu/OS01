/*
 * test/mock/fork_user_map/fork_user_map_runtime.h — Host runtime
 * stub for compiling the REAL kernel/memory/vma.c against the
 * fork-ownership host harness (test_fork_user_map.c, Task 6).
 *
 * Mirrors the pranges/brk/lifecycle pattern: the production vma.c
 * transitively includes heavy headers (<sched/task.h>,
 * <arch/spinlock.h>, <arch/cpu.h>, ...) which we sidestep here.
 *
 * Differences vs pranges: the fork test does NOT drive do_mmap /
 * do_munmap / do_mprotect — it only calls the VMA-list primitives
 * (fork_vma_copy, vma_free_all, vma_find, vma_insert, mm_alloc,
 * mm_init_user_heap) and inspects kernel/sched/task.c via
 * source-level reads.  Page-table mutation is observed via the
 * hand-rolled flat PTE table + alloc/free pool, not via
 * do_mmap's callsite.  TLB flush counters come from the host
 * stubs' tlb_shootdown() invocation.
 *
 * Force-include this header BEFORE any production header is
 * compiled, so the guard defines intercept the heavy path.
 */
#ifndef OS01_FORK_USER_MAP_RUNTIME_H
#define OS01_FORK_USER_MAP_RUNTIME_H

#include "test_platform.h"

/* ── Skip heavy production headers (mirror pranges pattern) ──── */
#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H
#endif
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H
#endif
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H
#endif
#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H
#endif
#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H
#endif
#ifndef _KERNEL_H
#define _KERNEL_H
#endif
#ifndef _MEMORY_SLAB_H
#define _MEMORY_SLAB_H
#endif
#ifndef _MEMORY_PMM_H
#define _MEMORY_PMM_H
#endif
#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H
#endif
#ifndef _MEMORY_UACCESS_H
#define _MEMORY_UACCESS_H
#endif
#ifndef _FS_FILE_H
#define _FS_FILE_H
#endif
#ifndef _ARCH_MMU_H          /* privileged CR3/invlpg inline asm */
#define _ARCH_MMU_H
#endif
/* Don't block _FS_VFS_H — vma.c needs the production typedef. */

#include <list.h>

/* ── arch/mmu.h replacements (counter-backed, no privileged asm) ──
 * fork_vma_copy / vma_free_all never invoke flush_tlb directly
 * (production code only flushes from do_munmap / do_mprotect), but
 * vmm_unmap_4k_page in our harness's stubs does bump tlb_shootdown
 * — keep the inline replacement so vma.c compiles. */
extern unsigned long fk_fork_tlb_shootdown_calls;

static inline uint64_t *arch_get_page_table(void) { return NULL; }
static inline void arch_flush_tlb_all(void)       { fk_fork_tlb_shootdown_calls++; }
static inline void arch_flush_tlb_page(uintptr_t v) { (void)v; fk_fork_tlb_shootdown_calls++; }
static inline void arch_switch_mm(uint64_t *pgdir) { (void)pgdir; }
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va)
{
    (void)pgtbl; (void)va;
    return 0;
}
static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                              uint64_t len, bool writable)
{
    (void)pgtbl; (void)addr; (void)len; (void)writable;
    return false;
}

/* ── mm_struct: only the fields vma.c dereferences ──────────── */
typedef struct mm_struct {
    uint64_t *pgdir;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;
    list_t   vma_list;
    uint64_t mmap_base;
    spinlock_T lock;
} mm_t;

/* ── task_struct: only the fields vma.c dereferences ────────── */
struct files_struct;
typedef struct files_struct files_t;

typedef struct task_struct {
    mm_t    *mm;
    files_t *files;
    uint64_t addr_limit;
} task_t;

/* `current` — the test never invokes do_mmap/do_munmap/do_mprotect
 * so a NULL stub is safe.  Some vma.c paths (do_munmap_locked,
 * do_mprotect) would deref `current->mm` if the test called them,
 * so they're off-limits. */
extern task_t *fk_current_task(void);
#define current fk_current_task()

/* ── Phy_To_Virt / Virt_To_Phy identity on host ─────────────── */
#ifndef Phy_To_Virt
#define Phy_To_Virt(addr) ((void *)(uintptr_t)(addr))
#endif
#ifndef Virt_To_Phy
#define Virt_To_Phy(addr) ((uintptr_t)(addr))
#endif

/* ── Forward-declare the slab functions vma.c uses ──────────── */
#include <stddef.h>
void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* OS01_FORK_USER_MAP_RUNTIME_H */