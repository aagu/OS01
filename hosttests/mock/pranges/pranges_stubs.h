/*
 * test/mock/pranges/pranges_stubs.h — Stubs that let the production
 * kernel/memory/vma.c compile and run for the protected-ranges test
 * (hosttests/cases/test_user_protected_ranges.c, Task 5).
 *
 * What is stubbed:
 *   - alloc_4k_page / free_4k_page: host-heap 4 KiB-aligned pool so
 *     the test can hand-fill PTEs for "preexisting mapping" checks
 *     and observe every alloc/free.
 *   - vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page: a flat
 *     indexed PTE table covering [0x400000, 0x400000 + 16 MiB).
 *   - page_cow_get / page_cow_put / page_cow_refs: no-op-ish
 *     refcount stubs (do_mprotect's COW branch is out of scope for
 *     Task 5 but must link).
 *   - vfs_node_get / vfs_node_put: identity / no-op.
 *   - tlb_shootdown: counter (mm_set_brk references it).
 *   - mm_user_range_protected: WEAK RED fallback returning false so
 *     the suite links and fails cleanly before Task 5's production
 *     implementation lands in kernel/memory/vma.c.
 *
 * What is NOT stubbed:
 *   - kmalloc / kfree: thin libc wrappers (also declared here).
 *   - Phy_To_Virt: identity (pranges_runtime.h).
 */
#ifndef OS01_PRANGES_STUBS_H
#define OS01_PRANGES_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)

/* ── Page-pool record (test assertion visibility) ───────────── */
#define PR_PAGE_POOL_SIZE 64
typedef struct {
    uint64_t phys;        /* simulated physical address */
    void    *backing;     /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
    uint16_t cow_refs;
} pr_page_record_t;

/* ── Flat PTE table covering [0x400000, 0x42100000) ───────────
 * Index = (va - 0x400000) >> 12.  Covers the whole user VA window
 * below the stack AND the auto-mmap region at mmap_base
 * (0x40000000) plus headroom, so hand-filled "preexisting
 * mapping" PTEs are observable there too. */
#define PR_PTE_TABLE_SIZE   0x42000UL
#define PR_PTE_USER_OFFSET  0x400000UL

typedef struct {
    uint64_t pte;           /* phys | PAGE_VALID etc */
    uint64_t phys;
    uint64_t flags;
} pr_pte_record_t;

typedef struct {
    pr_pte_record_t  ptes[PR_PTE_TABLE_SIZE];
    int total_allocs, total_frees;
    int total_maps, total_unmaps;
    int total_tlb_shootdowns;
} pr_state_t;

extern pr_state_t pr_state;
extern unsigned long pr_arch_tlb_flushes;

void pr_stubs_reset(void);
pr_page_record_t *pr_find_page(uint64_t phys);
pr_pte_record_t *pr_find_mapping(uint64_t va);
int pr_pte_index_for(uint64_t va);

/* ── current-task fixture control ─────────────────────────────
 * The harness's `current` points at a static task_t; the test
 * points it at the fixture mm (and a production-shaped addr_limit)
 * before driving do_mmap / do_munmap / do_mprotect. */
void pr_set_current(mm_t *mm, uint64_t addr_limit);
void pr_clear_current(void);

/* ── Stubs linked with the production vma.c ─────────────────── */
uint64_t alloc_4k_page(void);
void     free_4k_page(uint64_t phys);

uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);
int      vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                         uint64_t virt, uint64_t flags);
void     vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);

void     tlb_shootdown(void);
void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);
uint16_t page_cow_refs(uint64_t phys);

void *kmalloc(size_t size);
size_t kfree(void *ptr);

/* Forward-declared vfs_node_t (production <fs/vfs.h> typedef). */
struct vfs_node;
typedef struct vfs_node vfs_node_t;
vfs_node_t *vfs_node_get(vfs_node_t *node);
void        vfs_node_put(vfs_node_t *node);

#endif /* OS01_PRANGES_STUBS_H */
