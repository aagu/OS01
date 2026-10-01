/*
 * test/mock/brk/brk_runtime.h — Host runtime stub for compiling
 * the REAL kernel/memory/vma.c against the brk-page host harness
 * (test_brk_pages.c).
 *
 * Mirrors the lifecycle/elf_load patterns: the production vma.c
 * transitively includes heavy headers (<sched/task.h>,
 * <arch/spinlock.h>, <arch/cpu.h>, ...) which we sidestep here.
 *
 * Force-include this header BEFORE any production header is
 * compiled, so the guard defines intercept the heavy path.
 */
#ifndef OS01_BRK_RUNTIME_H
#define OS01_BRK_RUNTIME_H

#include "test_platform.h"

/* ── Skip heavy production headers (mirror lifecycle pattern) ─ */
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
/* Don't block _FS_VFS_H — vma.c needs the production typedef. */

#include <list.h>

/* ── mm_struct: only the fields vma.c dereferences ───────── */
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

/* ── task_struct: only the fields vma.c dereferences ──────── */
struct files_struct;
typedef struct files_struct files_t;

typedef struct task_struct {
    mm_t    *mm;
    files_t *files;
    uint64_t addr_limit;
} task_t;

/* `current` (production macro expanding to get_current_task())
 * is referenced by vma.c do_mmap/do_munmap/do_mprotect.  mm_set_brk
 * takes mm directly and does not touch `current` at all. */
extern task_t *brk_stub_current(void);
#define current brk_stub_current()

/* ── Phy_To_Virt / Virt_To_Phy identity on host ────────────
 * Production <memory/memory.h> redefines them with PAGE_OFFSET;
 * we skip that header (above) and just inherit identity from
 * <test_platform.h>. */
#ifndef Phy_To_Virt
#define Phy_To_Virt(addr) ((void *)(uintptr_t)(addr))
#endif
#ifndef Virt_To_Phy
#define Virt_To_Phy(addr) ((uintptr_t)(addr))
#endif

/* ── Forward-declare the slab functions vma.c uses ─────────── */
#include <stddef.h>
void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* OS01_BRK_RUNTIME_H */