/*
 * test/mock/uwrite_cow/uwrite_cow_stubs.h — Stub surface for the
 * user-write-cow harness (test_user_write_cow.c, Task 7,
 * user heap/ELF isolation plan).
 *
 * Compile-time policy:
 *   - Production arch/mmu.h is GUARDED OUT by uwrite_cow_runtime.h;
 *     this header is the test's only source of arch_user_range_accessible.
 *     The harness exposes a PTE-walking helper (uw_arch_range_accessible)
 *     that the production uaccess.c will eventually call into (Task 7
 *     must extend syscall_check_user_range to accept COW).  For now
 *     the test's RED step pins the helper to return false, forcing
 *     every "pure check accepts COW" assertion to fail until the
 *     production code is updated.
 *   - Production kernel/memory/vma.c IS linked (production-linked
 *     test, same as the brk/fork_user_map tests).  The harness stubs
 *     allocate/free, PTE walk, COW refcount, TLB-shootdown so every
 *     observable surface is counter-recorded.
 *
 * Test scaffolding:
 *   - 4 KiB page pool keyed by phys, with COW refcounts
 *   - Flat indexed PTE table covering [0x400000, 0x4400000)
 *   - Per-call counters (total_allocs, total_frees, total_maps,
 *     total_unmaps, total_cow_gets, total_cow_puts, total_tlbs,
 *     prepare_user_write_range_calls, prepare_failed_enomem,
 *     prepare_failed_efault)
 *   - Failure injection for alloc_4k_page at the Nth successful
 *     call (set uw_state.inject_alloc_fail_at; reset via
 *     uw_stubs_reset)
 */
#ifndef OS01_UWRITE_COW_STUBS_H
#define OS01_UWRITE_COW_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Page-pool record (test assertion visibility) ───────────── */
#define UW_PAGE_POOL_SIZE 256
typedef struct {
    uint64_t phys;        /* simulated physical address */
    void    *backing;     /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
    uint16_t cow_refs;    /* per-phys COW refcount (independent of PTE) */
} uw_page_record_t;

extern uw_page_record_t uw_page_pool[UW_PAGE_POOL_SIZE];
void     uw_stubs_reset(void);
uw_page_record_t *uw_find_page(uint64_t phys);
int      uw_in_use_count(void);

/* ── Flat PTE table for the user VA window ────────────────────
 * Covers [USER_CODE_ADDR, USER_CODE_ADDR + 1 MiB).  Each test uses
 * a unique 4 KiB slot; with identity Phy_To_Virt the phys IS the
 * user VA, so the mmap'd window provides the backing for actual
 * memcpy writes.  The size (256 pages) fits the page pool and
 * every test fixture (each test owns its own slot to avoid races
 * with previous tests' state). */
#define UW_PTE_TABLE_SIZE   0x100UL           /* 1 MiB / 4 KiB */
#define UW_PTE_USER_OFFSET  0x400000UL

typedef struct {
    uint64_t pte;           /* phys | PAGE_VALID etc */
    uint64_t phys;
    uint64_t flags;
} uw_pte_record_t;

typedef struct {
    uw_pte_record_t ptes[UW_PTE_TABLE_SIZE];

    /* Per-call counters (test assertions) */
    int total_maps;
    int total_unmaps;
    int total_allocs;
    int total_frees;
    int total_cow_puts;
    int total_cow_gets;
    int total_tlb_shootdowns;

    int inject_kmalloc_fail;

    /* Failure injection for alloc_4k_page: 0 disables; when set, the
     * Nth alloc_4k_page call returns 0 (OOM). */
    int inject_alloc_fail_at;

    /* Failure injection for vmm_map_4k_page (intermediate-table OOM). */
    int inject_map_fail_at;
} uw_state_t;

extern uw_state_t uw_state;

uw_pte_record_t *uw_find_mapping(uint64_t va);
int uw_pte_index_for(uint64_t va);

/* ── Stubs linked with production vma.c + uaccess.c ─────────── */
uint64_t  alloc_4k_page(void);
void      free_4k_page(uint64_t phys);

uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);
int       vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                           uint64_t virt, uint64_t flags);
void      vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);

void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);
uint16_t page_cow_refs(uint64_t phys);

void tlb_shootdown(void);

/* ── Test-only `current` driver ───────────────────────────────── */
void uw_set_current(mm_t *mm, uint64_t addr_limit);
void uw_clear_current(void);

/* ── Test-only PTE walker ──────────────────────────────────────
 * Returns true iff every page in [addr, addr+len) has PAGE_VALID
 * AND PAGE_USER.  Honours PAGE_COW for the writable=true case.
 * This mirrors what syscall_check_user_range + prepare_user_write_range
 * need to accept for the brief's "pure check accepts COW" rule. */
bool uw_arch_range_accessible(uint64_t addr, uint64_t len, bool writable);

/* tlb_shootdown counter (also bumped by mm_set_brk etc.) */
extern unsigned long uw_arch_flush_tlb_all_calls;

#endif /* OS01_UWRITE_COW_STUBS_H */