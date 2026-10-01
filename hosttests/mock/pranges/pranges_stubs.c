/*
 * test/mock/pranges/pranges_stubs.c — Stub implementations for the
 * protected-ranges host harness (test_user_protected_ranges.c,
 * Task 5, user heap/ELF isolation plan).
 *
 * The test compiles the REAL kernel/memory/vma.c so do_mmap /
 * do_munmap / do_mprotect / mm_user_range_protected are exercised
 * against observable state: a flat indexed PTE table, a 4 KiB
 * page pool with counters, and VMA-list walks.
 */
#include "pranges_stubs.h"
#include "pranges_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <memory/vmm.h>   /* PAGE_VALID, PAGE_COW, PAGE_PROTNONE ... */

/* posix_memalign (POSIX 1003.1-2001). */
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Global state ───────────────────────────────────────────── */
pr_state_t pr_state;
unsigned long pr_arch_tlb_flushes = 0;

/* The page pool lives OUTSIDE pr_state so pr_stubs_reset() can
 * wipe counters + PTEs without leaking the 4 KiB backings. */
static pr_page_record_t pr_pool[PR_PAGE_POOL_SIZE];
static int pool_initialised = 0;

static void pr_page_pool_init(void)
{
    if (pool_initialised) return;
    pool_initialised = 1;
    for (int i = 0; i < PR_PAGE_POOL_SIZE; i++) {
        void *p = NULL;
        if (posix_memalign(&p, 4096, 4096) != 0 || !p) abort();
        memset(p, 0, 4096);
        pr_pool[i].phys     = (uint64_t)(uintptr_t)p;
        pr_pool[i].backing  = p;
        pr_pool[i].in_use   = 0;
        pr_pool[i].cow_refs = 0;
    }
}

void pr_stubs_reset(void)
{
    pr_page_pool_init();
    memset(&pr_state, 0, sizeof(pr_state));
    for (int i = 0; i < PR_PAGE_POOL_SIZE; i++) {
        pr_pool[i].in_use   = 0;
        pr_pool[i].cow_refs = 0;
    }
    pr_arch_tlb_flushes = 0;
}

pr_page_record_t *pr_find_page(uint64_t phys)
{
    if (!pool_initialised) return NULL;
    for (int i = 0; i < PR_PAGE_POOL_SIZE; i++) {
        if (pr_pool[i].phys == phys) return &pr_pool[i];
    }
    return NULL;
}

/* ── Stub `current` ─────────────────────────────────────────── */
static task_t pr_task = { 0 };
task_t *pr_current_task(void) { return &pr_task; }

void pr_set_current(mm_t *mm, uint64_t addr_limit)
{
    pr_task.mm         = mm;
    pr_task.files      = NULL;
    pr_task.addr_limit = addr_limit;
}

void pr_clear_current(void)
{
    memset(&pr_task, 0, sizeof(pr_task));
}

/* ── kmalloc / kfree: thin libc wrappers ────────────────────── */
void *kmalloc(size_t size) { return malloc(size); }
size_t kfree(void *ptr)    { free(ptr); return 1; }

/* ── VFS stub ───────────────────────────────────────────────── */
vfs_node_t *vfs_node_get(vfs_node_t *node) { return node; }
void        vfs_node_put(vfs_node_t *node) { (void)node; }

/* ── tlb_shootdown (mm_set_brk references it; not driven here) ─ */
void tlb_shootdown(void) { pr_state.total_tlb_shootdowns++; }

/* ── mm_user_range_protected — WEAK RED fallback ──────────────
 * Returns false for everything until the production strong
 * definition lands in kernel/memory/vma.c.  With the fallback,
 * every "protected range is rejected" assertion fails — that is
 * the RED state.  Once the strong symbol exists it wins the link. */
__attribute__((weak)) bool mm_user_range_protected(
    const mm_t *mm, uint64_t start, uint64_t end)
{
    (void)mm; (void)start; (void)end;
    return false;
}

/* ── alloc_4k_page / free_4k_page ───────────────────────────── */
uint64_t alloc_4k_page(void)
{
    pr_page_pool_init();
    for (int i = 0; i < PR_PAGE_POOL_SIZE; i++) {
        if (!pr_pool[i].in_use) {
            pr_pool[i].in_use = 1;
            memset(pr_pool[i].backing, 0, 4096);
            pr_state.total_allocs++;
            return pr_pool[i].phys;
        }
    }
    return 0;
}

void free_4k_page(uint64_t phys)
{
    pr_page_pool_init();
    pr_page_record_t *rec = pr_find_page(phys);
    if (!rec) return;
    if (!rec->in_use) return;
    rec->in_use = 0;
    rec->cow_refs = 0;
    pr_state.total_frees++;
}

/* ── PTE table helpers ──────────────────────────────────────── */
int pr_pte_index_for(uint64_t va)
{
    if (va < PR_PTE_USER_OFFSET) return -1;
    uint64_t off = va - PR_PTE_USER_OFFSET;
    if (off >= (uint64_t)PR_PTE_TABLE_SIZE * 0x1000UL) return -1;
    return (int)(off >> 12);
}

pr_pte_record_t *pr_find_mapping(uint64_t va)
{
    int idx = pr_pte_index_for(va);
    if (idx < 0) return NULL;
    if (!(pr_state.ptes[idx].pte & PAGE_VALID)) return NULL;
    return &pr_state.ptes[idx];
}

/* ── COW refcount stubs (link surface only) ─────────────────── */
void page_cow_get(uint64_t phys)
{
    pr_page_record_t *rec = pr_find_page(phys);
    if (rec) rec->cow_refs++;
}

bool page_cow_put(uint64_t phys)
{
    pr_page_record_t *rec = pr_find_page(phys);
    if (rec && rec->cow_refs > 0) rec->cow_refs--;
    return true;
}

uint16_t page_cow_refs(uint64_t phys)
{
    pr_page_record_t *rec = pr_find_page(phys);
    return rec ? rec->cow_refs : 0;
}

/* ── vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page ──────── */
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)flags; (void)allocate;
    int idx = pr_pte_index_for(virt);
    if (idx < 0) return NULL;
    return &pr_state.ptes[idx].pte;
}

int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    (void)pgdir;
    int idx = pr_pte_index_for(virt);
    if (idx < 0) return -1;
    if (phys & ~PAGE_4K_MASK) return -1;
    pr_pte_record_t *rec = &pr_state.ptes[idx];
    rec->pte   = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    rec->phys  = phys;
    rec->flags = flags;
    pr_state.total_maps++;
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    int idx = pr_pte_index_for(virt);
    if (idx < 0) return;
    pr_pte_record_t *rec = &pr_state.ptes[idx];
    if (!(rec->pte & (PAGE_VALID | PAGE_PROTNONE))) return;
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
    pr_state.total_unmaps++;
}
