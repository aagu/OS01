/*
 * test/mock/pranges/pranges_runtime.h — Host runtime stub for
 * compiling the REAL kernel/memory/vma.c against the protected-
 * ranges host harness (test_user_protected_ranges.c, Task 5).
 *
 * Mirrors the brk/lifecycle pattern: the production vma.c
 * transitively includes heavy headers (<sched/task.h>,
 * <arch/spinlock.h>, <arch/cpu.h>, ...) which we sidestep here.
 *
 * Difference vs brk_runtime.h: this harness DRIVES do_mmap /
 * do_munmap / do_mprotect, so it must also neutralise the
 * privileged inline asm in <arch/mmu.h> (arch_flush_tlb_all
 * reloads CR3 — instant SIGSEGV in ring 3).  We block _ARCH_MMU_H
 * and provide counter-backed stubs for the six static inline
 * functions vma.c's TU references.
 *
 * Force-include this header BEFORE any production header is
 * compiled, so the guard defines intercept the heavy path.
 */
#ifndef OS01_PRANGES_RUNTIME_H
#define OS01_PRANGES_RUNTIME_H

#include "test_platform.h"

/* ── Skip heavy production headers (mirror brk pattern) ────── */
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
 * The production vmm.h maps flush_tlb() → arch_flush_tlb_all();
 * do_munmap / do_mprotect call it on success.  Counting the calls
 * lets the test observe the "no partial mutation" contract (a
 * rejected request must not reach the flush at the end). */
extern unsigned long pr_arch_tlb_flushes;

static inline uint64_t *arch_get_page_table(void) { return NULL; }
static inline void arch_flush_tlb_all(void)       { pr_arch_tlb_flushes++; }
static inline void arch_flush_tlb_page(uintptr_t v) { (void)v; pr_arch_tlb_flushes++; }
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

/* `current` — the test populates the returned task via
 * pr_set_current(mm, addr_limit) before driving the do_mmap
 * family. */
extern task_t *pr_current_task(void);
#define current pr_current_task()

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

#endif /* OS01_PRANGES_RUNTIME_H */
