/*
 * test/mock/lifecycle/lifecycle_stubs.h — Stubs that let the
 * production kernel/memory/vma.c compile and run for the process-
 * image lifecycle test (test_process_image_lifecycle.c).
 *
 * What is stubbed:
 *   - alloc_4k_page / free_4k_page: pulled from a host-heap
 *     4 KiB-aligned pool so vma_free_all's vmm_unmap_4k_page
 *     calls resolve to observable, test-verifiable frees.
 *   - vfs_node_get / vfs_node_put: no-op reference counting
 *     (the test never inserts a VMA with vm_file != NULL).
 *   - page_cow_refs / page_cow_put: no-op (vma.c only uses them
 *     in do_mprotect, which the lifecycle test does not call).
 *
 * What is NOT stubbed (production headers suffice on the host):
 *   - kmalloc / kfree:  hosttests/mock/mock_kernel.c.
 *   - Phy_To_Virt:      hosttests/mock/lifecycle/lifecycle_runtime.h
 *                       (identity on host).
 */
#ifndef OS01_LIFECYCLE_STUBS_H
#define OS01_LIFECYCLE_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <fs/file.h>           // file_t / files_t for the MAP_FIXED fd path

/* Forward-declared vfs_node_t so we don't pull in the heavy
 * production <fs/vfs.h> from this header (the host harness only
 * needs the opaque pointer for the stub signatures). */
struct vfs_node;
typedef struct vfs_node vfs_node_t;

/* alloc_4k_page / free_4k_page — 4 KiB-aligned host-heap pool.
 * Each alloc tags a 4 KiB buffer with a simulated phys = host
 * pointer cast; free records the release for test verification. */
#define LIFECYCLE_PAGE_POOL_SIZE 64
typedef struct {
    uint64_t phys;       /* simulated physical address */
    void    *backing;    /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
} lifecycle_page_record_t;

extern lifecycle_page_record_t lifecycle_page_pool[LIFECYCLE_PAGE_POOL_SIZE];

/* alloc/free/ref/sync counters (test assertions).
 *
 * The mprotect range tests drive the production do_mprotect through
 * this harness, so they need to observe more than the page pool:
 *   - vfs_get_calls / vfs_put_calls: file references taken by VMA
 *     splits and dropped by vma_remove;
 *   - cow_put_calls: proves the new mprotect path never drops a COW
 *     reference (COW privatization is deferred to first write);
 *   - flush_count / lock_count / unlock_count: commit ordering. */
typedef struct {
    int total_allocs;
    int total_frees;
    int vfs_get_calls;
    int vfs_put_calls;
    int cow_put_calls;
    int flush_count;
    int lock_count;
    int unlock_count;
    int files_get_calls;
    int files_put_calls;
} lifecycle_state_t;

extern lifecycle_state_t lifecycle_state;

/* ── Fake 4 KiB page table for do_mprotect ────────────────────
 * vmm_pt_walk() returns the registered slot for a virtual address
 * (or NULL, i.e. an empty slot).  Only registered VAs are visible,
 * so the tests never have to build a real four-level tree. */
#define LIFECYCLE_PTE_SLOTS 32

void      lifecycle_pte_reset(void);
void      lifecycle_pte_set(uint64_t va, uint64_t value);
uint64_t *lifecycle_pte_get(uint64_t va);       /* NULL = empty slot */

/* ── COW reference table ─────────────────────────────────────
 * page_cow_refs() returns the registered count (1 for an unknown
 * page, preserving the old stub contract) and page_cow_put()
 * decrements it, counting every call. */
#define LIFECYCLE_COW_SLOTS 16

void lifecycle_cow_reset(void);
void lifecycle_cow_set(uint64_t phys, uint16_t refs);

/* ── Sync event log ──────────────────────────────────────────
 * The order of LOCK / UNLOCK / FLUSH events proves the commit
 * order required by the spec: PTEs are updated before the flush,
 * and the flush happens before mm->lock is released. */
enum {
    LIFECYCLE_EV_LOCK = 1,
    LIFECYCLE_EV_UNLOCK = 2,
    LIFECYCLE_EV_FLUSH = 3,
};

#define LIFECYCLE_EVENT_SLOTS 64

int  lifecycle_event_count(void);
int  lifecycle_event_at(int index);

/* PTE value captured when the flush hook fired (0 with *valid = 0
 * when the VA was not registered at that moment). */
uint64_t lifecycle_flush_snapshot(uint64_t va, int *valid);

/* total_frees at the moment of the last TLB flush: proves data pages
 * are reclaimed only AFTER the page-table commit is synchronized. */
int lifecycle_flush_free_count(void);

/* Force the intermediate-table preparer to fail (0 = succeed).  The
 * real preparer is covered by test_x86_vmm_backend; the lifecycle
 * fixture uses this knob to drive the device transaction's prepare
 * failure path. */
void lifecycle_set_prepare_fail(int rc);

/* Model one user 2 MiB block leaf at `block` (2 MiB aligned), or
 * disable with block = 0.  Used by the fully-covered huge-block
 * withdrawal checks. */
void lifecycle_set_has_2m(uint64_t block);

/* ── Fake descriptor table (MAP_FIXED fd path) ───────────────
 * do_mmap pins the descriptor with files_get_file(), then takes the
 * node reference.  The harness provides a one-table stub so the
 * lifecycle tests can drive that path; the real fd table is covered
 * by the QEMU systest. */
void     lifecycle_files_reset(void);
files_t *lifecycle_files_table(void);
int      lifecycle_files_install(int fd, file_t *f);

/* Reset all counters + page pool to initial state. */
void lifecycle_stubs_reset(void);

/* Find a pool record by phys; returns NULL if unknown. */
lifecycle_page_record_t *lifecycle_find_page(uint64_t phys);

/* ── Stubs linked with the production vma.c ─────────────── */
uint64_t alloc_4k_page(void);
void     free_4k_page(uint64_t phys);

/* Match the production signatures (kernel/include/fs/vfs.h).
 * The host stubs return the node unchanged / no-op; the test
 * never inserts a VMA with vm_file != NULL, so vfs_node_put's
 * decrement path is never exercised. */
vfs_node_t *vfs_node_get(vfs_node_t *node);
void        vfs_node_put(vfs_node_t *node);

bool     page_cow_put(uint64_t phys);
uint16_t page_cow_refs(uint64_t phys);

/* ── Test page table for the stack-page single-free assertion ──
 *
 * Brief Step 1: "Assert stack-page failure has exactly one free."
 * This is a count assertion about the runtime release chain
 * (NOT a source-text invariant), so the host harness must drive
 * the cleanup path and observe the page-pool counter.
 *
 * We give the test a minimal fake user page table:
 *   test_user_pgd[0]  → test_user_pud_page[0]  → test_user_pmd_page[0]
 *
 * The PMD entry at index 0 covers VA [0, 2 MiB); USER_STACK_BASE =
 * 0x1400000 sits in [2 MiB, 4 MiB) → PMD index 1 (covered by a
 * second PMD page, see test_user_pmd_stack[]).
 *
 * The test sets test_user_pmd_stack[0] = phys | PAGE_USER_PMD |
 * PAGE_VALID before calling vmm_free_user_map; the stub walks the
 * structure and calls free_4k_page(phys) exactly once. */
#define LIFECYCLE_PGD_SIZE   512
#define LIFECYCLE_PUD_SIZE   512
#define LIFECYCLE_PMD_SIZE   512

/* Allocated 4 KiB-aligned via posix_memalign (see lifecycle_stubs.c)
 * so PAGE_4K_MASK in the walker preserves the pointer value. */
extern uint64_t *test_user_pgd;
extern uint64_t *test_user_pud_page;
extern uint64_t *test_user_pmd_lo;
extern uint64_t *test_user_pmd_stack;

#endif /* OS01_LIFECYCLE_STUBS_H */
