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
#include <memory/vmm.h>   /* PAGE_VALID, PAGE_HUGE, PAGE_NO_EXEC, PAGE_USER_PMD */

/* posix_memalign (POSIX 1003.1-2001) — see elf_load_stubs.c for
 * the rationale on declaring it explicitly. */
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Fake user page table for the stack-page single-free test ──
 * See lifecycle_stubs.h for the structure layout.  These arrays are
 * the only state the stub vmm_free_user_map understands; passing
 * any other pointer is a no-op.  test_user_pgd[0] is the only
 * non-zero PGD entry; test_user_pud_page[0] is the only non-zero
 * PUD entry; test_user_pmd_lo covers VA [0, 2 MiB) and
 * test_user_pmd_stack covers VA [2 MiB, 4 MiB) (USER_STACK_BASE
 * lives at 2 MiB-aligned 0x1400000, so PMD index 1 is where the
 * stack mapping sits).
 *
 * Allocated 4 KiB-aligned via posix_memalign so PAGE_4K_MASK in
 * the walker preserves the pointer value. */
uint64_t *test_user_pgd;
uint64_t *test_user_pud_page;
uint64_t *test_user_pmd_lo;
uint64_t *test_user_pmd_stack;

static int pgdir_allocated = 0;

static void allocate_test_pgdir(void)
{
    if (pgdir_allocated) return;
    pgdir_allocated = 1;
    posix_memalign((void **)&test_user_pgd,       4096, LIFECYCLE_PGD_SIZE * sizeof(uint64_t));
    posix_memalign((void **)&test_user_pud_page,  4096, LIFECYCLE_PUD_SIZE * sizeof(uint64_t));
    posix_memalign((void **)&test_user_pmd_lo,    4096, LIFECYCLE_PMD_SIZE * sizeof(uint64_t));
    posix_memalign((void **)&test_user_pmd_stack, 4096, LIFECYCLE_PMD_SIZE * sizeof(uint64_t));
}

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

/* ── vmm_unmap_4k_page / vmm_pt_walk / vmm_map_4k_page ─────
 * Stubs so vma.c's link succeeds.  The lifecycle test does not
 * invoke do_mmap / do_munmap / do_mprotect — only mm_init_user_heap
 * and vma_free_all.  vma_free_all calls vmm_unmap_4k_page once per
 * 4 KiB page in each VMA range; the heap VMA is zero-length so the
 * loop body never runs.  Provide no-ops anyway.  vmm_map_4k_page and
 * tlb_shootdown are referenced by mm_set_brk (Task 4); the lifecycle
 * test never calls mm_set_brk, but the production vma.c still needs
 * the symbols at link time. */
void     vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir; (void)virt;
}
int      vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                         uint64_t virt, uint64_t flags)
{
    (void)pgdir; (void)phys; (void)virt; (void)flags;
    return 0;
}
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)virt; (void)flags; (void)allocate;
    return NULL;
}

/* mm_set_brk (Task 4) references tlb_shootdown; the lifecycle test
 * never calls mm_set_brk, but the link needs the symbol. */
void tlb_shootdown(void) { (void)0; }

/* ── vmm_free_user_map — minimal page-table walker ──────────
 * The destroy_unpublished_user_mm helper calls vmm_free_user_map
 * to release the user page tables.  For the lifecycle test we
 * model the production walker (kernel/memory/vmm.c:130-176) at
 * minimum fidelity: handle the PAGE_HUGE branch the user stack
 * lives in (the spawn flow maps the stack as PAGE_USER_PMD =
 * 2 MiB huge page at USER_STACK_BASE = 0x1400000).
 *
 * The walker only knows about test_user_pgd / test_user_pud_page /
 * test_user_pmd_lo / test_user_pmd_stack — passing any other pgdir
 * pointer is a no-op.  This is sufficient for the lifecycle test:
 * a single PMD huge leaf is enough to exercise the single-free
 * invariant. */
void vmm_free_user_map(uint64_t *pgdir)
{
    allocate_test_pgdir();
    if (pgdir != test_user_pgd) return;

    /* The walker only walks level 0 of the PGD — production
     * code uses 512-way at each level, but our fake pgdir has
     * exactly one entry (index 0) pointing at test_user_pud_page. */
    if (!(test_user_pgd[0] & PAGE_VALID)) return;
    uint64_t *pud = (uint64_t *)(uintptr_t)(test_user_pgd[0] & PAGE_4K_MASK);
    if (pud != test_user_pud_page) return;

    /* PUD entry 0 → test_user_pmd_lo covers VA [0, 2 MiB).
     * PUD entry 1 → test_user_pmd_stack covers VA [2 MiB, 4 MiB)
     * (USER_STACK_BASE = 0x1400000 ∈ [2 MiB, 4 MiB) →
     *  PMD index 0 in test_user_pmd_stack). */
    for (int l1 = 0; l1 < 2; l1++) {
        uint64_t *pmd;
        if (l1 == 0) {
            if (!(test_user_pud_page[0] & PAGE_VALID)) continue;
            uint64_t *p = (uint64_t *)(uintptr_t)(test_user_pud_page[0] & PAGE_4K_MASK);
            if (p != test_user_pmd_lo) continue;
            pmd = test_user_pmd_lo;
        } else {
            if (!(test_user_pud_page[1] & PAGE_VALID)) continue;
            uint64_t *p = (uint64_t *)(uintptr_t)(test_user_pud_page[1] & PAGE_4K_MASK);
            if (p != test_user_pmd_stack) continue;
            pmd = test_user_pmd_stack;
        }
        for (int l2 = 0; l2 < LIFECYCLE_PMD_SIZE; l2++) {
            uint64_t pmde = pmd[l2];
            if (!(pmde & PAGE_VALID)) continue;
            if (!(pmde & PAGE_HUGE)) continue;
            /* Production extracts phys via PAGE_2M_MASK (2 MiB-aligned).
             * For the host harness we want the exact phys the test
             * recorded (a host-heap pointer, not 2 MiB-aligned), so
             * we mask off only the PAGE_NO_EXEC bit (bit 63) and the
             * flag bits.  The test puts `stack_phys | PAGE_USER_PMD`
             * into the PMD entry, and PAGE_USER_PMD overlaps only the
             * valid/user/write/huge bits (low 8 bits + bit 63), so
             * masking `pmde & ~(PAGE_NO_EXEC | 0xFF)` recovers the
             * original phys cleanly. */
            uint64_t phys = pmde & ~((uint64_t)PAGE_NO_EXEC | 0xFFULL);
            free_4k_page(phys);
            /* Clear the entry so a second walk (e.g. an idempotent
             * re-destroy) does not double-free.  Production
             * vmm_free_user_map does the same at vmm.c:160-164. */
            pmd[l2] = 0;
        }
    }
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
