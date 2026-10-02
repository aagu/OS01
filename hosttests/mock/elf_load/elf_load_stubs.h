/*
 * test/mock/elf_load/elf_load_stubs.h — Host stubs that let the
 * production kernel/fs/elf.c compile and run for the per-4 KiB
 * loader test (hosttests/cases/test_elf_load_4k.c).
 *
 * The host harness links the REAL kernel/fs/elf.c (not a duplicate
 * formula) so the loader's branch coverage and rollback discipline
 * are exercised against an observable, scriptable PTE state machine.
 *
 * What is stubbed (and why):
 *   - vfs_read: returns bytes from the in-memory ELF byte buffer.
 *     Supports offset+len bounds checks like the real VFS.
 *   - alloc_4k_page / free_4k_page: pulls 4 KiB buffers from a
 *     host-heap pool.  alloc returns 0 on inject-fail, free
 *     records every release so tests can verify rollback frees
 *     each created leaf exactly once.
 *   - vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page: a real,
 *     index-keyed PTE table that lets the test observe flags and
 *     verify no PAGE_HUGE leaf was ever created.
 *
 * What is NOT stubbed (production headers suffice on the host):
 *   - kmalloc/kfree: from hosttests/mock/mock_kernel.c.
 *   - Phy_To_Virt: from hosttests/mock/test_platform.h (identity).
 *   - memset/memcpy: libc.
 *
 * Failure injection:
 *   The test sets inject_alloc_fail_at / inject_read_fail_at /
 *   inject_map_fail_at to the count at which the corresponding
 *   stub should start returning failure.  0 disables the injection.
 *   All counters reset per test via stubs_reset().
 */
#ifndef OS01_ELF_LOAD_STUBS_H
#define OS01_ELF_LOAD_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Page-pool record (test assertion visibility) ───────────
 * Each alloc_4k_page() picks a host-heap buffer and tags it with a
 * simulated phys address (= the host pointer cast).  Tests inspect
 * the backing buffer to verify BSS bytes stay zero, shared-page
 * payloads survive the segment copy, etc. */
typedef struct {
    uint64_t phys;       /* simulated physical address */
    void    *backing;    /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
} page_pool_record_t;

#define ELF_LOAD_PAGE_POOL_SIZE 4096
extern page_pool_record_t elf_load_page_pool[ELF_LOAD_PAGE_POOL_SIZE];

/* Page-table entry storage (the test's "real PTE state machine").
 *
 * We allocate a single flat table indexed by (vaddr - USER_CODE_ADDR)
 * >> 12, sized to cover the entire user VA window
 * [USER_CODE_ADDR, USER_CODE_ADDR + USER_PAGE_SIZE).  Each slot is
 * one PTE-shaped uint64_t: present bit + phys + flags.  The mock
 * vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page operate on this
 * table; the production loader never sees a PAGE_HUGE bit because
 * the table is flat (no PMD leaves).  PAGE_HUGE bit is asserted to
 * be unset on every recorded mapping via the test.
 *
 * Index 0 = first user page at USER_CODE_ADDR (0x400000). */
#define ELF_LOAD_PTE_USER_OFFSET  0x400000UL
#define ELF_LOAD_PTE_TABLE_SIZE   4096         /* covers [0x400000, 0x1400000) */

typedef struct {
    uint64_t pte;          /* phys | flags (PAGE_VALID set if present) */
    uint64_t phys;         /* the phys the loader asked us to map */
    uint64_t flags;        /* the flags the loader asked us to set */
} elf_load_pte_record_t;

typedef struct {
    /* PTE table: indexed by (vaddr - USER_CODE_ADDR) >> 12. */
    elf_load_pte_record_t ptes[ELF_LOAD_PTE_TABLE_SIZE];

    /* Per-leaf lifecycle counters. */
    int total_maps;        /* total successful vmm_map_4k_page calls */
    int total_unmaps;      /* total successful vmm_unmap_4k_page calls */
    int total_allocs;      /* total successful alloc_4k_page calls */
    int total_frees;       /* total successful free_4k_page calls */
    int total_reads;       /* total successful vfs_read calls */

    /* Failure injection controls: when the matching call counter
     * reaches this value (on success path), the NEXT call returns
     * failure instead.  0 disables injection. */
    int inject_alloc_fail_at;  /* inject failure on Nth successful alloc_4k_page (after N succeed) */
    int inject_read_fail_at;  /* inject failure on Nth successful vfs_read */
    int inject_map_fail_at;    /* inject failure on Nth successful vmm_map_4k_page */
} elf_load_state_t;

/* Global state — single-instance test (no threading). */
extern elf_load_state_t elf_load_state;

/* Reset all counters + failure injection controls. */
void stubs_reset(void);

/* Lookup a recorded mapping (test assertions).  vaddr must be a
 * 4 KiB-aligned user VA.  Returns pointer to the PTE record, or
 * NULL if vaddr is out of range or never mapped. */
elf_load_pte_record_t *stubs_find_mapping(uint64_t vaddr);

/* Compute the PTE-table index for vaddr; returns -1 if vaddr is
 * outside the user range. */
int stubs_pte_index_for(uint64_t vaddr);

/* ── Failure injection API ───────────────────────────────
 * inject_alloc_fail_at / inject_read_fail_at / inject_map_fail_at
 * are public fields; tests set them directly via elf_load_state. */

/* ── Stubs linked with the production elf.c ─────────────── */
#include <fs/vfs.h>        /* vfs_node_t definition for stub signature */

/* Implemented in elf_load_stubs.c. */
int       vfs_read(vfs_node_t *node, uint64_t offset,
                   uint64_t size, void *buffer);

uint64_t  alloc_4k_page(void);
void      free_4k_page(uint64_t phys);

uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);
int       vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                           uint64_t virt, uint64_t flags);
void      vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);

/* Production log.h declares these.  The test framework's debug_fs
 * gate (OS01_DEBUG_fs) is 0 so log_debug is never called; we only
 * need g_log_level defined (declared extern in log.h). */

#endif /* OS01_ELF_LOAD_STUBS_H */