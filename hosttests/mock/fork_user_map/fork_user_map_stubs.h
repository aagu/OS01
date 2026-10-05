/*
 * test/mock/fork_user_map/fork_user_map_stubs.h — Stubs that let
 * the production kernel/memory/vma.c compile and run for the
 * fork-ownership host test (hosttests/cases/test_fork_user_map.c,
 * Task 6, user heap/ELF isolation plan).
 *
 * What is stubbed (and why):
 *   - alloc_4k_page / free_4k_page: 4 KiB-aligned host-heap pool
 *     keyed by phys — every alloc/free is counted so the test
 *     observes vma_free_all + fork_vma_copy's leaf handling.
 *   - vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page: flat
 *     indexed PTE table covering [0x400000, 0x42100000). The fork
 *     test hand-fills PTEs to model "parent has ELF/heap/mmap
 *     leaves committed before fork".
 *   - page_cow_get / page_cow_put / page_cow_refs: per-phys
 *     refcounting so vmm_unmap_4k_page's COW-aware branch is
 *     observable (decrement + free-on-zero).
 *   - vfs_node_get / vfs_node_put: refcount counting — the
 *     file-backed VMA test exercises vma_remove's
 *     vfs_node_put on the LAST reference.
 *   - tlb_shootdown: counter (mm_set_brk / production vma.c
 *     transitively reference it).
 *
 * What is NOT stubbed (production headers suffice on the host):
 *   - kmalloc / kfree: thin libc wrappers (also declared here).
 *   - Phy_To_Virt: identity (fork_user_map_runtime.h).
 */
#ifndef OS01_FORK_USER_MAP_STUBS_H
#define OS01_FORK_USER_MAP_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)

/* ── Page-pool record (test assertion visibility) ───────────── */
#define FK_PAGE_POOL_SIZE 256
typedef struct {
    uint64_t phys;        /* simulated physical address */
    void    *backing;     /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
    uint16_t cow_refs;    /* per-phys COW refcount (independent of PTE) */
    int      file_ref_count; /* vfs_node_put on the LAST reference for the
                              * file-backed VMA teardown */
} fk_page_record_t;

extern fk_page_record_t fk_page_pool[FK_PAGE_POOL_SIZE];

void     fk_stubs_reset(void);
fk_page_record_t *fk_find_page(uint64_t phys);
int      fk_in_use_count(void);  /* total pages currently in_use */

/* ── Flat PTE table for the user VA window ────────────────────
 * Covers [USER_CODE_ADDR, USER_CODE_ADDR + 16 MiB + heap window)
 * page count = 0x42000 — same window as pranges; the fork test
 * only needs the heap + ELF window (stack is huge-page handled
 * by fork_mm_copy separately). */
#define FK_PTE_TABLE_SIZE   0x42000UL
#define FK_PTE_USER_OFFSET  0x400000UL

typedef struct {
    uint64_t pte;           /* phys | PAGE_VALID etc */
    uint64_t phys;
    uint64_t flags;
} fk_pte_record_t;

typedef struct {
    fk_pte_record_t ptes[FK_PTE_TABLE_SIZE];

    /* Per-call counters (test assertions) */
    int total_maps;         /* successful vmm_map_4k_page calls */
    int total_unmaps;       /* successful vmm_unmap_4k_page calls */
    int total_allocs;       /* successful alloc_4k_page calls */
    int total_frees;        /* successful free_4k_page calls */
    int total_cow_puts;     /* page_cow_put calls reaching 0 */
    int total_cow_gets;     /* page_cow_get calls */
    int total_tlb_shootdowns;  /* tlb_shootdown() calls (SMP TLB sync) */
} fk_state_t;

extern fk_state_t fk_state;
extern unsigned long fk_fork_tlb_shootdown_calls;

/* Lookup a recorded mapping (test assertions).  va must be a
 * 4 KiB-aligned user VA inside the test window.  Returns NULL if
 * vaddr is out of range or never mapped. */
fk_pte_record_t *fk_find_mapping(uint64_t va);

/* Compute the PTE-table index for va; returns -1 if va is outside
 * the test window. */
int fk_pte_index_for(uint64_t va);

/* ── Stubs linked with the production vma.c ─────────────── */

uint64_t  alloc_4k_page(void);
void      free_4k_page(uint64_t phys);

uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);
int       vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                           uint64_t virt, uint64_t flags);
void      vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);

/* COW refcount helpers — keyed by phys via the page pool. */
void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);    /* returns true when count reaches 0 */
uint16_t page_cow_refs(uint64_t phys);

/* tlb_shootdown — SMP-aware TLB flush (vma.c transitively
 * references it via mm_set_brk).  Counted so the test can verify
 * the contract. */
void tlb_shootdown(void);

/* Forward-declared vfs_node_t so we don't pull in <fs/vfs.h> */
struct vfs_node;
typedef struct vfs_node vfs_node_t;
vfs_node_t *vfs_node_get(vfs_node_t *node);
void        vfs_node_put(vfs_node_t *node);

/* File-backed VMA refcount for vfs_node_get/put tracking. */
int fk_node_get_count(void);
int fk_node_put_count(void);

#endif /* OS01_FORK_USER_MAP_STUBS_H */