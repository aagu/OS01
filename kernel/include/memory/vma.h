#ifndef _KERNEL_VMA_H
#define _KERNEL_VMA_H

/*
 * Public, arch-neutral VMA metadata (mm-architecture-boundaries Task 3).
 *
 * This header carries ONLY the shared logical permission flags, the
 * vma_t structure and the MAP/PROT syscall constants.  It must NOT
 * transitively provide page-table implementation details (no VMM or
 * MMU include).  The architecture-owned operations live in the per-arch
 * headers:
 *   - x86:   <arch/x86_64/vma.h>   (mmap/heap/fork/teardown + leaf
 *                                   conversion)
 *   - ARM:   <arch/aarch64/user_vm_policy.h> (managed-root policy)
 * and the shared linked-list primitives stay in <memory/vm_area.h>.
 */

#include <stdint.h>
#include <stdbool.h>
#include <list.h>
#include <memory/mm.h>   // mm_alloc (compat forward — moved out of this header)
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
                              // NOT the vmm.h VM_* PTE-bit family).
                              // The SOLE source of leaf permissions: the
                              // owning architecture derives hardware PTE
                              // bits on demand (x86_vma_pte_flags).
    uint64_t    vm_pgoff;     // file offset in 4KB pages
    vfs_node_t *vm_file;      // NULL = anonymous
} vma_t;

#endif // _KERNEL_VMA_H
