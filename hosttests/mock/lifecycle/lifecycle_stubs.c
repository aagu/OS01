/*
 * test/mock/lifecycle/lifecycle_stubs.c — Stub implementations
 * for compiling and running the production kernel/memory/vma.c
 * against the host harness (test_process_image_lifecycle).
 *
 * Uses -Wl,--wrap=kmalloc so the test TU can inject kmalloc
 * failures via __wrap_kmalloc while the production vma.c still
 * sees a normal kmalloc symbol.
 */
#include "lifecycle_stubs.h"
#include "lifecycle_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <memory/pmm.h>

/* posix_memalign (POSIX 1003.1-2001) — see elf_load_stubs.c for
 * the rationale on declaring it explicitly. */
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Stub `current` (lifecycle_runtime.h redefines the macro) ──
 * The test never invokes do_mmap / do_munmap / do_mprotect, so the
 * .mm / .files / .addr_limit fields are never read.  Use a static
 * zero-initialised task_t — accessing any field returns NULL/0. */
static task_t stub_current_task = { 0 };
task_t *current_task_for_host_test(void) { return &stub_current_task; }

/* ── kmalloc / kfree ────────────────────────────────────────
 * These symbols provide the "real" kmalloc / kfree that the test's
 * __wrap_kmalloc / __wrap_kfree wrap (linker flag:
 *   -Wl,--wrap=kmalloc,--wrap=kfree).
 *
 * Production <memory/slab.h> declares them; we skip the header to
 * avoid its full impl surface.  Implementation: thin wrappers over
 * libc malloc / free, satisfying the production vma.c call sites.
 *
 * NOTE: do NOT link mock_kernel.o into this binary — its own
 * kmalloc / kfree symbols would clash with these. */
void *kmalloc(size_t size)        { return malloc(size); }
size_t kfree(void *ptr)           { free(ptr); return 1; }

/* ── vmm_unmap_4k_page / vmm_pt_walk ────────────────────────
 * Stubs so vma.c's link succeeds.  The lifecycle test does not
 * invoke do_mmap / do_munmap / do_mprotect — only mm_init_user_heap
 * and vma_free_all.  vma_free_all calls vmm_unmap_4k_page once per
 * 4 KiB page in each VMA range; the heap VMA is zero-length so the
 * loop body never runs.  Provide a no-op anyway. */
void     vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir; (void)virt;
}
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)virt; (void)flags; (void)allocate;
    return NULL;
}

/* ── Global state ─────────────────────────────────────────── */
lifecycle_state_t lifecycle_state;
lifecycle_page_record_t lifecycle_page_pool[LIFECYCLE_PAGE_POOL_SIZE];

static int pool_initialised = 0;

static void pool_init_once(void)
{
    if (pool_initialised) return;
    pool_initialised = 1;
    for (int i = 0; i < LIFECYCLE_PAGE_POOL_SIZE; i++) {
        void *p = NULL;
        if (posix_memalign(&p, 4096, 4096) != 0 || !p) {
            abort();
        }
        memset(p, 0, 4096);
        lifecycle_page_pool[i].backing = p;
        lifecycle_page_pool[i].phys = (uint64_t)(uintptr_t)p;
        lifecycle_page_pool[i].in_use = 0;
    }
}

void lifecycle_stubs_reset(void)
{
    pool_init_once();
    memset(&lifecycle_state, 0, sizeof(lifecycle_state));
    /* Mark every page free (in_use=0); do NOT zero backing — the
     * test may have left non-zero data there. */
    for (int i = 0; i < LIFECYCLE_PAGE_POOL_SIZE; i++) {
        lifecycle_page_pool[i].in_use = 0;
    }
}

lifecycle_page_record_t *lifecycle_find_page(uint64_t phys)
{
    pool_init_once();
    for (int i = 0; i < LIFECYCLE_PAGE_POOL_SIZE; i++) {
        if (lifecycle_page_pool[i].phys == phys)
            return &lifecycle_page_pool[i];
    }
    return NULL;
}

/* ── alloc_4k_page / free_4k_page ─────────────────────────── */
uint64_t alloc_4k_page(void)
{
    pool_init_once();
    for (int i = 0; i < LIFECYCLE_PAGE_POOL_SIZE; i++) {
        if (!lifecycle_page_pool[i].in_use) {
            lifecycle_page_pool[i].in_use = 1;
            memset(lifecycle_page_pool[i].backing, 0, 4096);
            lifecycle_state.total_allocs++;
            return lifecycle_page_pool[i].phys;
        }
    }
    /* Pool exhausted — production returns 0 (OOM).  The test never
     * allocates more than ~16 pages, so this is purely defensive. */
    return 0;
}

void free_4k_page(uint64_t phys)
{
    pool_init_once();
    lifecycle_page_record_t *rec = lifecycle_find_page(phys);
    if (!rec) {
        /* Freeing unknown phys — production would kpanic; host just
         * drops it.  Should not happen in this test. */
        return;
    }
    if (!rec->in_use) {
        /* Double-free: drop silently. */
        return;
    }
    rec->in_use = 0;
    lifecycle_state.total_frees++;
}

/* ── vfs_node_get / vfs_node_put — no-op reference counting ──
 * The lifecycle test never inserts a VMA with vm_file != NULL,
 * so the production vma_remove path that calls vfs_node_put is
 * never exercised.  Provide stubs so the link succeeds. */
vfs_node_t *vfs_node_get(vfs_node_t *node) { return node; }
void        vfs_node_put(vfs_node_t *node) { (void)node; }

/* ── page_cow_refs / page_cow_put — no-op ───────────────────
 * The lifecycle test does not exercise do_mprotect, which is the
 * only vma.c path that touches page_cow_refs/put.  Provide stubs
 * so the link succeeds; the test never invokes them. */
bool     page_cow_put(uint64_t phys)   { (void)phys; return true; }
uint16_t page_cow_refs(uint64_t phys)  { (void)phys; return 1; }
