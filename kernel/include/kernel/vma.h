#ifndef _KERNEL_VMA_H
#define _KERNEL_VMA_H

#include <stdint.h>
#include <list.h>
#include <kernel/vmm.h>
#include <fs/vfs.h>

// Forward declarations (avoids circular task.h ↔ vma.h)
struct mm_struct;
typedef struct mm_struct mm_t;

// ── VMA flags ──────────────────────────────────────────────
#define VM_READ      0x01
#define VM_WRITE     0x02
#define VM_EXEC      0x04
#define VM_SHARED    0x08
#define VM_MAYSHARE  0x10
#define VM_ANON      0x20   // anonymous mapping (no backing file)
#define VM_GROWSDOWN 0x40   // reserved, not implemented

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
    uint64_t    vm_flags;     // VM_*
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

// ── Syscall implementations (called from trap.c) ───────────
int64_t   do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset);
int64_t   do_mprotect(uint64_t addr, uint64_t length, uint64_t prot);
int64_t   do_munmap(uint64_t addr, uint64_t length);

#endif // _KERNEL_VMA_H
