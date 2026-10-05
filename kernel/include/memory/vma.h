#ifndef _KERNEL_VMA_H
#define _KERNEL_VMA_H

#include <stdint.h>
#include <stdbool.h>
#include <list.h>
#include <memory/vmm.h>
#include <fs/vfs.h>

// Forward declarations (avoids circular task.h ↔ vma.h)
struct mm_struct;
typedef struct mm_struct mm_t;

// ── VMA flags ──────────────────────────────────────────────
#define VMA_PROT_READ      0x01
#define VMA_PROT_WRITE     0x02
#define VMA_PROT_EXEC      0x04
#define VMA_SHARED    0x08
#define VMA_MAYSHARE  0x10
#define VMA_ANON      0x20   // anonymous mapping (no backing file)
#define VMA_GROWSDOWN 0x40   // reserved, not implemented
#define VMA_IO        0x80   // MMIO region (no COW, no file-backed demand paging)
#define VMA_HEAP      0x100  // the unique heap VMA — zero-length, tracks
                            // [start_brk, ALIGN_UP(end_brk, 4096))

// ── PROT_* constants (kernel-accessible copy of libc mman.h) ─
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// ── MAP_* constants ────────────────────────────────────────
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

// ── VMA structure ──────────────────────────────────────────
typedef struct vm_area_struct {
    list_t      list;
    uint64_t    vm_start;     // start VA (4KB aligned)
    uint64_t    vm_end;       // end VA (4KB aligned, exclusive)
    uint64_t    vm_flags;     // VMA_* flags (VMA_PROT_READ/WRITE/EXEC,
                              // VMA_SHARED/ANON/... — see VMA_* above;
                              // NOT the vmm.h VM_* PTE-bit family)
    uint64_t    vm_page_prot; // PAGE_* flags for PTE
    uint64_t    vm_pgoff;     // file offset in 4KB pages
    vfs_node_t *vm_file;      // NULL = anonymous
} vma_t;

// ── VMA operations ─────────────────────────────────────────
vma_t    *vma_find(mm_t *mm, uint64_t addr);
int       vma_insert(mm_t *mm, vma_t *vma);
void      vma_remove(mm_t *mm, vma_t *vma);
void      vma_free_all(mm_t *mm);
vma_t    *fork_vma_copy(mm_t *child_mm, mm_t *parent_mm);
mm_t     *mm_alloc(void);   // allocate + init an mm_t (lock = unlocked)

// ── mm_init_user_heap — heap-VMA initializer ────────────────
// Must be called after a successful elf_load() on the new mm.
// Sets mm->start_brk = mm->end_brk = ALIGN_UP(elf_end, 4096).
// Inserts ONE zero-length heap VMA [start_brk, start_brk) with
//   vm_flags     = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON | VMA_HEAP,
//   vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID.
// Returns 0 on success, -ENOMEM if the VMA allocation fails.
// On failure: mm is unchanged (caller owns it; will destroy via
// destroy_unpublished_user_mm from kernel/sched/task.c).
//
// The zero-length VMA survives VMA traversal — vma_find() does
// NOT match it (vma_find checks `addr < vm_end`, which is always
// false for an empty range).  The heap VMA exists so brk-grown
// 4 KiB pages can be inserted by the brk syscall with a known
// owning VMA, and so fork() can reproduce the heap range via the
// fork_vma_copy path.  See docs/.../2026-10-01-user-heap-elf-
// isolation-design.md §4 (zero-length heap invariant).
int       mm_init_user_heap(mm_t *mm, uint64_t elf_end);

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
//
//   [0x400000,        mm->start_brk)   ELF reserve envelope — the
//                                     loaded image INCLUDING the
//                                     inter-segment gaps; not every
//                                     page in the envelope is mapped,
//                                     but the whole envelope is
//                                     protected.
//   [mm->start_brk,   0x13ff000)       heap reserve — the entire
//                                     brk window, not just the
//                                     committed pages; covers the
//                                     zero-length VMA_HEAP VMA's
//                                     range too.  The upper bound is
//                                     FIXED at 0x13ff000 — it does
//                                     NOT track end_brk.
//   [0x13ff000,       0x1400000)       heap→stack guard page.
//   [USER_STACK_BASE, +0x200000)       the 2 MiB user stack.
//
// Caller MUST:
//   - Pass a page-aligned interval (4 KiB boundaries, half-open).
//   - Validate overflow and the user-bounds check (e.g.
//     addr < current->addr_limit) BEFORE this call.
//   - When this returns true, reject the whole operation with
//     -EINVAL and perform NO mutation: no PTE change, no VMA
//     split/insert/remove, no do_munmap_locked, no device mmap
//     callback.
//
// The predicate takes NO lock: it only reads mm->start_brk, so
// callers may hold mm->lock around it (fixed-path) or call it
// unlocked and then take the lock (auto-search path) — both
// patterns are valid.
//
// mm == NULL or mm->start_brk == 0 (no user image installed —
// init_mm, kthreads) protects nothing.
bool mm_user_range_protected(const mm_t *mm, uint64_t start, uint64_t end);

// ── Syscall implementations (called from trap.c) ───────────
int64_t   do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset);
int64_t   do_mprotect(uint64_t addr, uint64_t length, uint64_t prot);
int64_t   do_munmap(uint64_t addr, uint64_t length);

#endif // _KERNEL_VMA_H
