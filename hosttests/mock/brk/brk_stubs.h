/*
 * test/mock/brk/brk_stubs.h — Stubs that let the production
 * kernel/memory/vma.c compile and run for the brk-page ownership
 * test (hosttests/cases/test_brk_pages.c).
 *
 * What is stubbed (and why):
 *   - alloc_4k_page / free_4k_page: 4 KiB-aligned host-heap pool
 *     so the test can observe every alloc/free of a fresh heap leaf.
 *   - vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page: a flat
 *     indexed PTE table that supports PAGE_VALID + PAGE_COW +
 *     PAGE_PROTNONE flag bits (no PAGE_HUGE — brk must never
 *     touch a 2 MiB leaf).
 *   - page_cow_get / page_cow_put / page_cow_refs: per-phys
 *     reference counting so the COW shrink / COW fault paths
 *     produce observable state changes.
 *   - arch_flush_tlb_all / flush_tlb: a counter incremented on
 *     every call so the test verifies mm_set_brk flushes SMP TLBs.
 *
 * What is NOT stubbed (production headers suffice on the host):
 *   - kmalloc / kfree:  hosttests/mock/mock_kernel.c
 *   - Phy_To_Virt:      hosttests/mock/brk/brk_runtime.h (identity)
 *   - memset / memcpy:  libc.
 */
#ifndef OS01_BRK_STUBS_H
#define OS01_BRK_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Page-pool record (test assertion visibility) ─────────── */
#define BRK_PAGE_POOL_SIZE 4096   /* covers [0x400000, 0x1400000) */
typedef struct {
    uint64_t phys;        /* simulated physical address */
    void    *backing;     /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
    uint16_t cow_refs;    /* per-phys COW refcount (independent of PTE) */
} brk_page_record_t;

extern brk_page_record_t brk_page_pool[BRK_PAGE_POOL_SIZE];

void     brk_page_pool_init(void);
void     brk_stubs_reset(void);
brk_page_record_t *brk_find_page(uint64_t phys);

/* ── Flat PTE table for the heap range ─────────────────────
 *
 * The PTE table covers [USER_CODE_ADDR, USER_CODE_ADDR+USER_PAGE_SIZE).
 * Each slot holds the PTE bits (phys | flags) for one 4 KiB leaf.
 *
 * Index = (va - USER_CODE_ADDR) >> 12 (no PAGE_HUGE — brk never
 * creates huge leaves).  Heap PAGE_* are taken from production
 * <memory/vmm.h> via the runtime header. */
#define BRK_PTE_TABLE_SIZE   4096          /* 16 MiB / 4 KiB */
#define BRK_PTE_USER_OFFSET  0x400000UL

typedef struct {
    uint64_t pte;           /* phys | PAGE_VALID etc */
    uint64_t phys;          /* the phys the loader asks us to map */
    uint64_t flags;         /* the flags the loader asks us to set */
} brk_pte_record_t;

typedef struct {
    brk_pte_record_t ptes[BRK_PTE_TABLE_SIZE];

    /* Per-call counters (test assertions) */
    int total_maps;         /* successful vmm_map_4k_page calls */
    int total_unmaps;       /* successful vmm_unmap_4k_page calls */
    int total_allocs;       /* successful alloc_4k_page calls */
    int total_frees;        /* successful free_4k_page calls */
    int total_cow_puts;     /* page_cow_put calls reaching 0 */
    int total_cow_gets;     /* page_cow_get calls */
    int total_tlb_shootdowns;  /* tlb_shootdown() calls (SMP TLB sync) */

    /* Failure injection: when the matching counter reaches this
     * value, the NEXT call returns failure instead.  0 disables. */
    int inject_alloc_fail_at;  /* inject failure on Nth successful alloc_4k_page */
    int inject_map_fail_at;    /* inject failure on Nth successful vmm_map_4k_page */
} brk_state_t;

extern brk_state_t brk_state;

/* Lookup a recorded mapping (test assertions).  va must be a
 * 4 KiB-aligned user VA inside the test window.  Returns NULL if
 * vaddr is out of range or never mapped. */
brk_pte_record_t *brk_find_mapping(uint64_t va);

/* Compute the PTE-table index for va; returns -1 if va is outside
 * the test window. */
int brk_pte_index_for(uint64_t va);

/* ── Stubs linked with the production vma.c ─────────────── */

uint64_t  alloc_4k_page(void);
void      free_4k_page(uint64_t phys);

uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);
int       vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                           uint64_t virt, uint64_t flags);
void      vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);

/* arch_flush_tlb_all / flush_tlb counters */
/* The production vmm.h defines flush_tlb() as arch_flush_tlb_all()
 * (a static inline in arch/mmu.h, gated by __x86_64__).  We do NOT
 * declare arch_flush_tlb_all here — production code that calls
 * flush_tlb() resolves to the inline asm.  mm_set_brk itself calls
 * tlb_shootdown() (the SMP-aware variant) which we DO stub, so the
 * test can observe the SMP-synchronization contract via a counter
 * on brk_tlb_shootdown_calls. */

/* COW refcount helpers — keyed by phys via the page pool. */
void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);    /* returns true when count reaches 0 */
uint16_t page_cow_refs(uint64_t phys);

/* tlb_shootdown — SMP-aware TLB flush (mm_set_brk contract).
 * Production vmm.h declares `void tlb_shootdown(void);` and the
 * real implementation lives in kernel/memory/tlb.c (not compiled
 * for the host harness).  We stub it here so the test can observe
 * mm_set_brk's SMP-synchronization calls via a counter. */
void tlb_shootdown(void);

/* Forward-declared vfs_node_t so we don't pull in <fs/vfs.h> */
struct vfs_node;
typedef struct vfs_node vfs_node_t;
vfs_node_t *vfs_node_get(vfs_node_t *node);
void        vfs_node_put(vfs_node_t *node);

#endif /* OS01_BRK_STUBS_H */