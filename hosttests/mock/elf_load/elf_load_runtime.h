/*
 * test/mock/elf_load/elf_load_runtime.h — Host runtime stub for
 * compiling the REAL kernel/fs/elf.c against the host harness
 * (test_elf_load_4k).
 *
 * elf.c includes the production <fs/vfs.h>, <sched/task.h>,
 * <memory/memory.h>, <memory/vmm.h>, <memory/pmm.h>, <core/debug.h>
 * and <arch/elf.h>.  On the host, several of those chains re-define
 * symbols (rdtsc, spinlock_T, spin_init, ...) that test_platform.h
 * already stubs as no-ops.  We avoid the conflict by skipping the
 * heavyweight production headers via their include guards and
 * providing minimal equivalents here:
 *
 *   * <sched/task.h>    — skipped; mm_t (the only field set the
 *                          loader touches) is declared directly
 *                          below with a layout compatible with
 *                          the production struct.
 *   * <arch/cpu.h>      — skipped; test_platform.h's rdtsc/hlt
 *                          stubs are sufficient.
 *   * <arch/spinlock.h> — skipped; test_platform.h's spinlock_T
 *                          and spin_* inlines are sufficient.
 *
 * Force-include this header (via ELF_LOAD_HOST_CFLAGS) before the
 * production source is compiled.
 */
#ifndef OS01_ELF_LOAD_RUNTIME_H
#define OS01_ELF_LOAD_RUNTIME_H

#include "test_platform.h"

/* Skip heavyweight production headers — guard defines must match
 * the header guards in kernel/include/. */
#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H
#endif
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H
#endif
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H
#endif
#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H
#endif

#ifndef PF_LINUX_ABI
#define PF_LINUX_ABI (1 << 3)
#endif

/* list_t for mm_t.vma_list — provided by libc/include/list.h on
 * the include path.  We only need the forward type for sizeof;
 * the loader never dereferences vma_list. */
#include <list.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)

/* ── Slab/kmalloc declarations ─────────────────────────────
 *
 * Production kernel/sched/task.c includes <memory/slab.h> to
 * declare kmalloc/kfree; elf.c relied on transitive inclusion via
 * sched/task.h.  Since we skip task.h, declare them here so the
 * production elf.c still finds a known symbol.  The host harness
 * links against hosttests/mock/mock_kernel.c which provides the
 * actual implementation (malloc/free wrappers). */
#include <stddef.h>
void *kmalloc(size_t size);
size_t kfree(void *ptr);

/* ── mm_struct (loader-only fields) ─────────────────────────
 *
 * Layout mirrors kernel/include/sched/task.h:78 (production).
 * Only the fields the loader reads or writes are semantically
 * important — the rest are sized to keep offsetof / sizeof
 * stable for any TU that includes both this and the production
 * mm_t (none today, but defensive). */
typedef struct mm_struct {
    uint64_t *pgdir;        /* physical address of PGD (mm_t.pgdir) */
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;
    list_t   vma_list;
    uint64_t mmap_base;
    spinlock_T lock;        /* test_platform.h provides spinlock_T */
} mm_t;

/* ── Stubs referenced via the include chain ──────────────────
 *
 * The host harness does not link any kernel scheduler code, so
 * declarations pulled in by other headers (memory/uaccess.h,
 * memory/vmm.h, fs/vfs.h, fs/file.h) need either to be skipped
 * or to have their non-inline implementations stubbed.  Most
 * chains reach only the headers we do NOT skip above; the
 * declarations those headers expose are satisfied by:
 *
 *   - kmalloc/kfree:           hosttests/mock/mock_kernel.c
 *   - vfs_read / alloc_4k_page / free_4k_page /
 *     vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page:
 *                               hosttests/mock/elf_load/elf_load_stubs.c
 *
 * Remaining declarations (task_wake, schedule, current, ...) are
 * not referenced by elf.c and are therefore not required at link
 * time for this test binary.
 */

/* ── Phy_To_Virt: identity on the host ──────────────────────
 *
 * Production <memory/memory.h> defines
 *   Phy_To_Virt(addr) = addr + PAGE_OFFSET (0xffff800000000000 on
 *   x86_64).  That address arithmetic makes sense for kernel
 *   direct-mapping but breaks the host harness: the loader's
 *   allocator returns host-heap pointers (e.g. 0x555555594060),
 *   and the production macro turns them into
 *   0xffff800000000000 + 0x555555594060 — an unmapped address that
 *   SIGSEGVs the first memset / vfs_read of the loaded bytes.
 *
 * test_platform.h defines Phy_To_Virt as identity, but
 * memory/memory.h re-defines it AFTER our force-include runs.
 * Pull in memory/memory.h here (the production definition runs),
 * then re-assert the identity macro so the rest of the compile
 * unit — including elf.c's transitive #include of memory/memory.h
 * (which is now a no-op via the header guard) — sees identity. */
#include <memory/memory.h>
#undef Phy_To_Virt
#define Phy_To_Virt(addr) ((void *)(uintptr_t)(addr))
#undef Virt_To_Phy
#define Virt_To_Phy(addr) ((uintptr_t)(addr))

#endif /* OS01_ELF_LOAD_RUNTIME_H */