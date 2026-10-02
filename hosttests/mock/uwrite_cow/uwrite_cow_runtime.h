/*
 * test/mock/uwrite_cow/uwrite_cow_runtime.h — Host runtime stub
 * for compiling the REAL kernel/memory/uaccess.c + vma.c against
 * the user-write-cow harness (test_user_write_cow.c, Task 7,
 * user heap/ELF isolation plan).
 *
 * Mirrors the brk/fork_user_map pattern: production uaccess.c
 * transitively includes heavy headers (<sched/task.h>,
 * <arch/spinlock.h>, ...) which we sidestep here with guard
 * defines.  The test exercises:
 *   - prepare_user_write_range(_locked)
 *   - copy_to_user_ft_res with on_fault callback
 *   - syscall_check_user_range with writable=true on COW leaves
 *   - arch_user_range_accessible with PAGE_COW
 *
 * The page pool + flat PTE table mirror fork_user_map.  The new
 * touch points are:
 *   - prepare_user_write_range must NOT allocate when all leaves
 *     are non-COW (counter check: total_allocs unchanged)
 *   - it MUST allocate a fresh phys for each COW leaf with
 *     refs > 1 (counter check: total_allocs increments by N)
 *   - on -ENOMEM mid-preparation, EVERY staged phys must be
 *     freed and the original PTEs/refcounts left untouched
 *
 * Force-include this header BEFORE any production header is
 * compiled, so the guard defines intercept the heavy path.
 */
#ifndef OS01_UWRITE_COW_RUNTIME_H
#define OS01_UWRITE_COW_RUNTIME_H

#define spin_lock uw_noop_spin_lock
#define spin_unlock uw_noop_spin_unlock
#define spin_lock_irqsave uw_noop_spin_lock_irqsave
#define spin_unlock_irqrestore uw_noop_spin_unlock_irqrestore
#include "test_platform.h"
#undef spin_lock
#undef spin_unlock
#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
static inline void spin_lock(spinlock_T *l) { if (l->lock) __builtin_trap(); l->lock = 1; }
static inline void spin_unlock(spinlock_T *l) { if (!l->lock) __builtin_trap(); l->lock = 0; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { spin_lock(l); return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)f; spin_unlock(l); }

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
#ifndef _FS_FILE_H
#define _FS_FILE_H
#endif
#ifndef _ARCH_MMU_H          /* privileged CR3/invlpg inline asm */
#define _ARCH_MMU_H
#endif
/* Don't block _FS_VFS_H — vma.c needs the production typedef. */

#include <list.h>

/* ── arch/mmu.h replacements (counter-backed, no privileged asm) ──
 * The test does NOT exercise arch_flush_tlb / arch_switch_mm,
 * but production headers still need the symbols to compile. */
extern unsigned long uw_arch_flush_tlb_all_calls;

static inline uint64_t *arch_get_page_table(void) { return NULL; }
static inline void arch_flush_tlb_all(void)       { uw_arch_flush_tlb_all_calls++; }
static inline void arch_flush_tlb_page(uintptr_t v) { (void)v; uw_arch_flush_tlb_all_calls++; }
static inline void arch_switch_mm(uint64_t *pgdir) { (void)pgdir; }
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va)
{
    (void)pgtbl; (void)va;
    return 0;
}

/* Architectural permission walking is mocked here; test_uaccess.c verifies
 * the production upper-level walker in QEMU. The syscall/VMA gate is real. */
static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                              uint64_t len, bool writable)
{
    extern bool uw_arch_range_accessible(uint64_t, uint64_t, bool);
    (void)pgtbl;
    return uw_arch_range_accessible(addr, len, writable);
}

/* ── mm_struct: only the fields uaccess.c/vma.c dereference ───── */
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

/* ── task_struct: only the fields uaccess.c dereference ───────── */
struct files_struct;
typedef struct files_struct files_t;

typedef struct task_struct {
    mm_t    *mm;
    files_t *files;
    uint64_t addr_limit;
    void   **fault_jmp;
    void    (*fault_cleanup)(void *);
    void    *fault_cleanup_arg;
} task_t;

extern task_t *uw_current_task(void);
#define current uw_current_task()

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

/* Forward-declare page helpers (defined in uwrite_cow_stubs.c). */
uint64_t alloc_4k_page(void);
void     free_4k_page(uint64_t phys);
void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);
uint16_t page_cow_refs(uint64_t phys);
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                       uint64_t flags, int allocate);
int      vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                          uint64_t virt, uint64_t flags);
void     vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);
void     tlb_shootdown(void);

#endif /* OS01_UWRITE_COW_RUNTIME_H */