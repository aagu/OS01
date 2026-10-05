/*
 * test/mock/fork_user_map/fork_user_map_stubs.c — Stub
 * implementations for the fork-ownership host harness
 * (hosttests/cases/test_fork_user_map.c, Task 6).
 *
 * The test compiles the REAL kernel/memory/vma.c so fork_vma_copy
 * and vma_free_all run against observable state.  The page pool,
 * flat PTE table, COW refcount, and vfs_node refcount give every
 * observable surface.  source-level inspection of
 * kernel/sched/task.c covers the staged ordering and the
 * do_fork teardown on fork_mm_copy failure (the task.c
 * dependency tree is too heavy to host-link).
 */
#include "fork_user_map_stubs.h"
#include "fork_user_map_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)   /* PAGE_VALID, PAGE_USER, PAGE_WRITE, PAGE_COW, PAGE_PROTNONE */

/* posix_memalign (POSIX 1003.1-2001) — declared explicitly so we
 * can use posix_memalign without dragging in <stdlib.h>'s deps. */
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Global state ───────────────────────────────────────────── */
fk_state_t fk_state;
fk_page_record_t fk_page_pool[FK_PAGE_POOL_SIZE];
unsigned long fk_fork_tlb_shootdown_calls = 0;

static int pool_initialised = 0;
static int node_get_count = 0;
static int node_put_count = 0;

/* ── Page-pool init + helpers ────────────────────────────── */
static void fk_page_pool_init(void)
{
    if (pool_initialised) return;
    pool_initialised = 1;
    for (int i = 0; i < FK_PAGE_POOL_SIZE; i++) {
        void *p = NULL;
        if (posix_memalign(&p, 4096, 4096) != 0 || !p) {
            abort();
        }
        memset(p, 0, 4096);
        fk_page_pool[i].phys            = (uint64_t)(uintptr_t)p;
        fk_page_pool[i].backing         = p;
        fk_page_pool[i].in_use          = 0;
        fk_page_pool[i].cow_refs        = 0;
        fk_page_pool[i].file_ref_count  = 0;
    }
}

void fk_stubs_reset(void)
{
    fk_page_pool_init();
    memset(&fk_state, 0, sizeof(fk_state));
    for (int i = 0; i < FK_PAGE_POOL_SIZE; i++) {
        fk_page_pool[i].in_use          = 0;
        fk_page_pool[i].cow_refs        = 0;
        fk_page_pool[i].file_ref_count  = 0;
    }
    node_get_count = 0;
    node_put_count = 0;
    fk_fork_tlb_shootdown_calls = 0;
}

fk_page_record_t *fk_find_page(uint64_t phys)
{
    if (!pool_initialised) return NULL;
    for (int i = 0; i < FK_PAGE_POOL_SIZE; i++) {
        if (fk_page_pool[i].phys == phys) return &fk_page_pool[i];
    }
    return NULL;
}

int fk_in_use_count(void)
{
    int n = 0;
    if (!pool_initialised) return 0;
    for (int i = 0; i < FK_PAGE_POOL_SIZE; i++) {
        if (fk_page_pool[i].in_use) n++;
    }
    return n;
}

/* ── Stub `current` (never derefed by the fork test) ─────────── */
static task_t fk_stub_task = {0};
task_t *fk_current_task(void) { return &fk_stub_task; }

/* ── kmalloc / kfree: thin libc wrappers ───────────────────
 * Production vma.c sees normal kmalloc/kfree symbols. */
void *kmalloc(size_t size) { return malloc(size); }
size_t kfree(void *ptr)    { free(ptr); return 1; }

/* ── vfs_node_get / vfs_node_put — counter-backed ─────────
 * Production semantics: vfs_node_get increments the refcount,
 * vfs_node_put decrements.  The file-backed VMA teardown path
 * (vma_remove → vfs_node_put) is observable here. */
vfs_node_t *vfs_node_get(vfs_node_t *node)
{
    (void)node;
    node_get_count++;
    return node;
}
void vfs_node_put(vfs_node_t *node)
{
    (void)node;
    node_put_count++;
}
int fk_node_get_count(void) { return node_get_count; }
int fk_node_put_count(void) { return node_put_count; }

/* ── tlb_shootdown — SMP TLB flush counter ──────────────── */
void tlb_shootdown(void) { fk_state.total_tlb_shootdowns++; }

/* ── alloc_4k_page / free_4k_page ────────────────────────── */
uint64_t alloc_4k_page(void)
{
    fk_page_pool_init();
    for (int i = 0; i < FK_PAGE_POOL_SIZE; i++) {
        if (!fk_page_pool[i].in_use) {
            fk_page_pool[i].in_use = 1;
            memset(fk_page_pool[i].backing, 0, 4096);
            fk_state.total_allocs++;
            return fk_page_pool[i].phys;
        }
    }
    /* Pool exhausted — production returns 0 (OOM). */
    return 0;
}

void free_4k_page(uint64_t phys)
{
    fk_page_pool_init();
    fk_page_record_t *rec = fk_find_page(phys);
    if (!rec) return;
    if (!rec->in_use) return;  /* silent on double-free */
    rec->in_use = 0;
    rec->cow_refs = 0;
    fk_state.total_frees++;
}

/* ── PTE table helpers ───────────────────────────────────── */
int fk_pte_index_for(uint64_t va)
{
    if (va < FK_PTE_USER_OFFSET) return -1;
    uint64_t off = va - FK_PTE_USER_OFFSET;
    if (off >= (uint64_t)FK_PTE_TABLE_SIZE * 0x1000UL) return -1;
    return (int)(off >> 12);
}

fk_pte_record_t *fk_find_mapping(uint64_t va)
{
    int idx = fk_pte_index_for(va);
    if (idx < 0) return NULL;
    if (!(fk_state.ptes[idx].pte & PAGE_VALID)) return NULL;
    return &fk_state.ptes[idx];
}

/* ── COW refcount helpers ──────────────────────────────────
 * page_cow_get / page_cow_put / page_cow_refs — keyed by phys.
 * Production semantics: page_cow_put returns true when the count
 * reaches zero (caller typically frees the phys in that case). */
void page_cow_get(uint64_t phys)
{
    fk_page_pool_init();
    fk_page_record_t *rec = fk_find_page(phys);
    if (!rec) return;
    rec->cow_refs++;
    fk_state.total_cow_gets++;
}

bool page_cow_put(uint64_t phys)
{
    fk_page_pool_init();
    fk_page_record_t *rec = fk_find_page(phys);
    if (!rec) return true;
    if (rec->cow_refs > 0) rec->cow_refs--;
    if (rec->cow_refs == 0) {
        fk_state.total_cow_puts++;
        return true;
    }
    return false;
}

uint16_t page_cow_refs(uint64_t phys)
{
    fk_page_pool_init();
    fk_page_record_t *rec = fk_find_page(phys);
    if (!rec) return 0;
    return rec->cow_refs;
}

/* ── vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page ────
 * Flat indexed PTE table — no intermediate page-table levels,
 * no PAGE_HUGE leaves.  Fork_mm_copy's 4 KiB leaf path is
 * observable here. */
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)flags; (void)allocate;
    int idx = fk_pte_index_for(virt);
    if (idx < 0) return NULL;
    return &fk_state.ptes[idx].pte;
}

int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    (void)pgdir;
    int idx = fk_pte_index_for(virt);
    if (idx < 0) return -1;
    if (phys & ~PAGE_4K_MASK) return -1;
    fk_pte_record_t *rec = &fk_state.ptes[idx];
    rec->pte   = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    rec->phys  = phys;
    rec->flags = flags;
    fk_state.total_maps++;
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    int idx = fk_pte_index_for(virt);
    if (idx < 0) return;
    fk_pte_record_t *rec = &fk_state.ptes[idx];

    if (!(rec->pte & (PAGE_VALID | PAGE_PROTNONE)))
        return;

    uint64_t phys = rec->pte & PAGE_4K_MASK;

    if (rec->pte & PAGE_COW) {
        if (page_cow_put(phys))
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    rec->pte   = 0;
    rec->phys  = 0;
    rec->flags = 0;
    fk_state.total_unmaps++;
}

/* ── prepare_user_write_range_locked — WEAK fallback ──────────
 * Task 7: vma.c's user_write_range_begin delegates to
 * prepare_user_write_range_locked.  The fork_user_map test never
 * drives user_write_range_begin (it tests fork_vma_copy / vma_free_all
 * via observable state), so the production symbol is never called.
 * Weak fallback so the link succeeds. */
__attribute__((weak)) int prepare_user_write_range_locked(mm_t *mm, uint64_t addr,
                                                          size_t len)
{
    (void)mm; (void)addr; (void)len;
    return -EFAULT;
}