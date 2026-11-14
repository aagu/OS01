/*
 * test/mock/lifecycle/lifecycle_runtime.h — Host runtime stub for
 * compiling the REAL kernel/memory/vma.c against the host harness
 * (test_process_image_lifecycle).
 *
 * Mirrors the elf_load_runtime.h pattern: the production vma.c
 * transitively includes <sched/task.h>, <arch/spinlock.h>,
 * <arch/cpu.h>, <arch/irq.h>, <memory/memory.h>, <arch/mmu.h> etc.
 * which would either fail to parse (aarch64 inline asm when host
 * clang targets x86_64 with the wrong #ifdef path) or pull in
 * privileged inline asm we don't want to execute.  We skip those
 * heavyweight headers via guard defines and provide minimal
 * equivalents here.
 *
 * The host harness links the REAL kernel/memory/vma.c, so the
 * mm_init_user_heap / vma_find / vma_insert / vma_free_all
 * contracts are exercised against observable state.
 *
 * Force-include this header (via LIFECYCLE_HOST_CFLAGS) BEFORE any
 * production header is compiled.
 */
#ifndef OS01_LIFECYCLE_RUNTIME_H
#define OS01_LIFECYCLE_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Recording spinlock surface (mprotect commit-order tests) ──
 * test_platform.h provides a no-op spinlock.  The mprotect tests must
 * observe that the PTE commit happens BEFORE the TLB sync and that the
 * sync happens BEFORE mm->lock is released, so this harness replaces
 * the no-op surface with one that logs acquire/release.  Defining the
 * guard first keeps test_platform.h from redefining it. */
#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;

void lifecycle_sync_lock(void);
void lifecycle_sync_unlock(void);

static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l)
{
    (void)l;
    lifecycle_sync_lock();
}
static inline void spin_unlock(spinlock_T *l)
{
    (void)l;
    lifecycle_sync_unlock();
}
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    (void)l;
    lifecycle_sync_lock();
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    (void)l;
    (void)f;
    lifecycle_sync_unlock();
}
#endif /* OS01_HOST_SPINLOCK_T_PROVIDED */

#include "test_platform.h"

/* ── Skip heavy production headers (mirror elf_load pattern) ──
 * vma.c doesn't dereference any of the types these headers provide;
 * we only need mm_t (declared below with the production layout). */
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

/* ── Host replacement for <arch/mmu.h> ──────────────────────
 * The production header installs x86 CR3 inline asm
 * (arch_flush_tlb_all), which crashes a user-space host process.
 * vma.c needs only flush_tlb() == arch_flush_tlb_all() plus the
 * address-space constants, so skip the production header entirely
 * and route the flush through the lifecycle event log.  The
 * mprotect commit-order test reads that log AND the PTE snapshot
 * taken at flush time. */
#ifndef _ARCH_MMU_H
#define _ARCH_MMU_H
#define _ARCH_X86_64_MMU_H  /* host mock owns the x86 MMU primitives */
#define ARCH_PAGE_OFFSET 0xffff800000000000ULL

void lifecycle_sync_flush(void);

static inline void arch_flush_tlb_all(void)
{
    lifecycle_sync_flush();
}
static inline void arch_flush_tlb_page(uintptr_t vaddr) { (void)vaddr; }
static inline uint64_t *arch_get_page_table(void) { return (uint64_t *)0; }
static inline void arch_switch_mm(uint64_t *pgdir) { (void)pgdir; }
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va)
{
    (void)pgtbl;
    return va;
}
/* Only consumed by uaccess.c, which the lifecycle suite does not link;
 * kept so the header surface matches the production one. */
static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                              uint64_t len, bool writable)
{
    (void)pgtbl; (void)addr; (void)len; (void)writable;
    return true;
}
#endif /* _ARCH_MMU_H */

/* list_t for mm_t.vma_list — provided by libc/include/list.h. */
#include <list.h>

/* ── kmalloc/kfree declarations ────────────────────────────
 * Production <memory/slab.h> declares them; we skip the header to
 * avoid pulling its full impl surface.  mock_kernel.c provides the
 * actual implementation (malloc/free wrappers). */
#include <stddef.h>
void *kmalloc(size_t size);
size_t kfree(void *ptr);

/* ── mm_struct (vma.c only needs these fields) ─────────────
 * Layout mirrors kernel/include/sched/task.h:78 (production). */
typedef struct mm_struct {
    uint64_t *pgdir;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;
    list_t   vma_list;
    uint64_t mmap_base;
    spinlock_T lock;        /* test_platform.h provides spinlock_T */
} mm_t;

/* ── Minimal task_t (only the fields vma.c dereferences) ───
 * Production <sched/task.h> defines a 200+-byte task_t; vma.c
 * touches .mm / .files / .addr_limit only.  We declare those
 * three fields so the compiler accepts do_mmap / do_munmap /
 * do_mprotect / user_write_range_begin without dragging in the
 * rest. */
struct files_struct;
typedef struct files_struct files_t;

typedef struct task_struct {
    mm_t   *mm;
    files_t *files;
    uint64_t addr_limit;
} task_t;

/* `current` (production macro expanding to get_current_task())
 * is referenced everywhere in vma.c.  Redirect it to a static
 * stub — the test never invokes do_mmap / do_munmap / do_mprotect,
 * so the stub values are never read at runtime. */
extern task_t *current_task_for_host_test(void);
#define current current_task_for_host_test()

/* ── Phy_To_Virt / Virt_To_Phy identity on host ────────────
 * Production <memory/memory.h> re-defines them with
 *   Phy_To_Virt(addr) = addr + PAGE_OFFSET (0xffff800000000000)
 * which makes any host-heap pointer unmappable in the test.
 * We pull memory.h (its macros run), then re-assert identity.
 *
 * vma.c calls Phy_To_Virt() on mm->pgdir to get the user-pgd
 * pointer.  In the test, mm->pgdir is a host-heap pointer, so
 * identity Phy_To_Virt is essential for vma_free_all to find the
 * page table. */
#include <memory/memory.h>
#undef Phy_To_Virt
#define Phy_To_Virt(addr) ((void *)(uintptr_t)(addr))
#undef Virt_To_Phy
#define Virt_To_Phy(addr) ((uintptr_t)(addr))

#endif /* OS01_LIFECYCLE_RUNTIME_H */
