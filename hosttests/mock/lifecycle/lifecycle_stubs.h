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

/* alloc/free counters (test assertions) */
typedef struct {
    int total_allocs;
    int total_frees;
} lifecycle_state_t;

extern lifecycle_state_t lifecycle_state;

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

#endif /* OS01_LIFECYCLE_STUBS_H */
