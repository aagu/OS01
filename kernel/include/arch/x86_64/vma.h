#ifndef _ARCH_X86_64_VMA_H
#define _ARCH_X86_64_VMA_H

/*
 * x86_64 VMA owner surface (mm-architecture-boundaries Task 3).
 *
 * The x86 mmap/heap/fork/teardown operations and the leaf conversion.
 * The public <memory/vma.h> keeps only the logical flags, the vma_t
 * structure and the MAP/PROT constants; <memory/vm_area.h> keeps the
 * shared list primitives.  These declarations are the x86-only
 * transaction layer.
 */

#include <stdint.h>
#include <stdbool.h>
#include <memory/vma.h>
#include <memory/vm_area.h>

// ── x86 leaf conversion (Task 1) ─────────────────────────────
/* Convert validated VMA logical permission bits into an x86 user 4 KiB
 * PTE flag word:
 *   - zero permission  → USER-only template (no PRESENT; the PROTNONE
 *                        stash path adds PAGE_PROTNONE itself);
 *   - R / RW           → NX (PTE permissions plan);
 *   - RX / RWX         → executable (prot_to_vma_flags already granted
 *                        EXEC its implicit READ);
 *   - WRITE            → from the VMA. */
uint64_t x86_vma_pte_flags(uint64_t vm_flags);

// ── x86 VMA node lifecycle ───────────────────────────────────
// Destroy one VMA node (unlink + file-reference drop + node free).
// The file reference and the node stay x86-owned; the list unlinking
// is the shared vm_area_unlink() primitive.  Address lookups and
// inserts use the shared vm_area_lookup()/vm_area_insert_sorted().
void      x86_vma_destroy_node(vma_t *vma);
void      vma_free_all(mm_t *mm);
int       fork_vma_copy(mm_t *child_mm, mm_t *parent_mm);

// ── mm_init_user_heap — heap-VMA initializer ────────────────
// Must be called after a successful elf_load() on the new mm.
// Sets mm->start_brk = mm->end_brk = ALIGN_UP(elf_end, 4096).
// Inserts ONE zero-length heap VMA [start_brk, start_brk) with
//   vm_flags = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON | VMA_HEAP
// (heap data pages are NX; the leaf word is derived on demand).
// Returns 0 on success, -ENOMEM if the VMA allocation fails.
// On failure: mm is unchanged (caller owns it; will destroy via
// destroy_unpublished_user_mm from kernel/sched/task.c).
int mm_init_user_heap(mm_t *mm, uint64_t elf_end);

// ── mm_set_brk — program-break owner (Task 4) ─────────────
//
// Sets the program break:
//   requested == 0   → query current (returns 0 with *result = current end_brk)
//   requested < start_brk → -EINVAL, *result unchanged
//   requested > heap_limit → -ENOMEM, *result unchanged
//   [start_brk, heap_limit] → grow or shrink to requested.
//     * On grow: stage and map new 4 KiB leaves; any OOM → -ENOMEM
//       with no commit (old break, old VMA end, old PTEs all unchanged).
//     * On shrink across a page boundary: privatize the retained
//       COW tail (if COW and refs > 1) before zeroing, then unmap
//       released leaves via the COW-aware VMM path; flush SMP TLBs.
//       *result = requested on success; -ENOMEM on any failure (old
//       state restored).
//   *result is set to the new end_brk on success.
//
// Caller must NOT hold mm->lock; mm_set_brk takes it.
int mm_set_brk(mm_t *mm, uint64_t requested, uint64_t *result);

// ── mm_user_range_protected — reserved-window predicate (Task 5) ──
//
// Returns true iff the HALF-OPEN page range [start, end) intersects
// ANY of the four reserved user ranges:
//   [0x400000, mm->start_brk)                    ELF reserve envelope
//   [mm->start_brk, HEAP_LIMIT)                  heap reserve
//   [HEAP_LIMIT, USER_STACK_BASE)                guard page
//   [USER_STACK_BASE, +0x200000)                 the 2 MiB user stack
//
// Caller MUST pass a page-aligned half-open interval and validate
// overflow / user bounds first; on true the caller rejects the whole
// operation with -EINVAL and performs NO mutation.  Takes no lock;
// mm == NULL or mm->start_brk == 0 protects nothing.
bool mm_user_range_protected(const mm_t *mm, uint64_t start, uint64_t end);

// ── Syscall implementations (called from sys_mm.c) ──────────
int64_t do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                uint64_t flags, uint64_t fd, uint64_t offset);
int64_t do_mprotect(uint64_t addr, uint64_t length, uint64_t prot);
int64_t do_munmap(uint64_t addr, uint64_t length);

#endif /* _ARCH_X86_64_VMA_H */
