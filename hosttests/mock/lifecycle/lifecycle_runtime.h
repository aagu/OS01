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
