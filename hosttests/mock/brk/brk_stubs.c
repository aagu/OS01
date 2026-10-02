/*
 * test/mock/brk/brk_stubs.c — Stub implementations for the brk-
 * page ownership test (hosttests/cases/test_brk_pages.c).
 *
 * Strategy C (Hybrid): the test compiles the REAL
 * kernel/memory/vma.c so mm_set_brk is exercised against observable
 * state.  We provide:
 *   - 4 KiB-aligned host-heap page pool (page_pool_records +
 *     total_allocs + total_frees counters).
 *   - A flat indexed PTE table that records (pte, phys, flags)
 *     per user 4 KiB page (no PAGE_HUGE — brk never creates huge
 *     leaves).  vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page
 *     operate on this table.
 *   - Per-phys COW refcounting (page_cow_get / page_cow_put /
 *     page_cow_refs) so the COW shrink / COW fault paths produce
 *     observable state changes.
 *   - arch_flush_tlb_all counter so the test verifies mm_set_brk
 *     flushes SMP TLBs on grow, shrink and rollback paths.
 *
 * Failure injection: brk_state.inject_alloc_fail_at /
 * inject_map_fail_at cause the corresponding stub to start
 * returning failure at the Nth successful call.
 */
#include "brk_stubs.h"
#include "brk_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <memory/vmm.h>   /* PAGE_VALID, PAGE_USER, PAGE_WRITE, PAGE_COW, PAGE_PROTNONE */

/* posix_memalign (POSIX 1003.1-2001) — elf_load_stubs.c has the
 * same rationale for declaring it explicitly. */
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Global state ─────────────────────────────────────────── */
brk_state_t brk_state;
brk_page_record_t brk_page_pool[BRK_PAGE_POOL_SIZE];

static int pool_initialised = 0;

/* ── Page-pool init + helpers ────────────────────────────── */
void brk_page_pool_init(void)
{
    if (pool_initialised) return;
    pool_initialised = 1;
    for (int i = 0; i < BRK_PAGE_POOL_SIZE; i++) {
        void *p = NULL;
        if (posix_memalign(&p, 4096, 4096) != 0 || !p) {
            abort();
        }
        memset(p, 0, 4096);
        brk_page_pool[i].phys     = (uint64_t)(uintptr_t)p;
        brk_page_pool[i].backing  = p;
        brk_page_pool[i].in_use   = 0;
        brk_page_pool[i].cow_refs = 0;
    }
}

void brk_stubs_reset(void)
{
    brk_page_pool_init();
    memset(&brk_state, 0, sizeof(brk_state));
    for (int i = 0; i < BRK_PAGE_POOL_SIZE; i++) {
        brk_page_pool[i].in_use   = 0;
        brk_page_pool[i].cow_refs = 0;
    }
}

brk_page_record_t *brk_find_page(uint64_t phys)
{
    if (!pool_initialised) return NULL;
    for (int i = 0; i < BRK_PAGE_POOL_SIZE; i++) {
        if (brk_page_pool[i].phys == phys) return &brk_page_pool[i];
    }
    return NULL;
}

/* ── Stub `current` ───────────────────────────────────────── */
static task_t brk_stub_task = {0};
task_t *brk_stub_current(void) { return &brk_stub_task; }

/* ── kmalloc / kfree: thin libc wrappers ───────────────────
 * The test may use --wrap=kmalloc/kfree for OOM injection.  The
 * production vma.c sees normal kmalloc/kfree symbols. */
void *kmalloc(size_t size) { return malloc(size); }
size_t kfree(void *ptr)    { free(ptr); return 1; }

/* ── VFS stub (do_mmap/do_munmap never reach here, but vma.c
 * transitively references the symbols) ─────────────────────── */
vfs_node_t *vfs_node_get(vfs_node_t *node) { return node; }
void        vfs_node_put(vfs_node_t *node) { (void)node; }

/* ── mm_set_brk stub (RED fallback) ───────────────────────
 * Provided as a weak symbol so the production vma.c's strong
 * definition overrides it once mm_set_brk is implemented.  Until
 * then, every test case that calls mm_set_brk gets -ENOSYS and
 * fails — that's the RED state we want before the implementation
 * lands in kernel/memory/vma.c. */
__attribute__((weak)) int mm_set_brk(mm_t *mm, uint64_t requested,
                                     uint64_t *result)
{
    (void)mm; (void)requested; (void)result;
    return -ENOSYS;
}

/* ── prepare_user_write_range_locked — WEAK fallback ──────────
 * Task 7: vma.c's user_write_range_begin delegates to
 * prepare_user_write_range_locked.  The brk test never drives
 * user_write_range_begin, so the production symbol is never
 * called from this build.  The weak fallback exists only so the
 * link succeeds. */
__attribute__((weak)) int prepare_user_write_range_locked(mm_t *mm, uint64_t addr,
                                                          size_t len)
{
    (void)mm; (void)addr; (void)len;
    return -EFAULT;
}

/* ── tlb_shootdown — SMP TLB flush counter ────────────────
 * Production tlb_shootdown() lives in kernel/memory/tlb.c (not
 * compiled for the host).  mm_set_brk calls this after leaf-map
 * and leaf-unmap operations to honour the "synchronize SMP TLBs"
 * contract — see the brief.  The counter increments every call
 * so the test verifies mm_set_brk does the synchronization. */
void tlb_shootdown(void) { brk_state.total_tlb_shootdowns++; }

/* ── alloc_4k_page / free_4k_page ────────────────────────── */
uint64_t alloc_4k_page(void)
{
    brk_page_pool_init();
    /* Honour failure injection: AFTER N successful allocs, the
     * NEXT call returns 0 (OOM).  Tests use this to verify
     * mm_set_brk's OOM rollback frees staged leaves. */
    if (brk_state.inject_alloc_fail_at > 0 &&
        brk_state.total_allocs + 1 == brk_state.inject_alloc_fail_at) {
        return 0;
    }
    for (int i = 0; i < BRK_PAGE_POOL_SIZE; i++) {
        if (!brk_page_pool[i].in_use) {
            brk_page_pool[i].in_use = 1;
            /* Zero the backing so every freshly-committed heap
             * leaf starts at zero. */
            memset(brk_page_pool[i].backing, 0, 4096);
            brk_state.total_allocs++;
            return brk_page_pool[i].phys;
        }
    }
    /* Pool exhausted — production returns 0 (OOM). */
    return 0;
}

void free_4k_page(uint64_t phys)
{
    brk_page_pool_init();
    brk_page_record_t *rec = brk_find_page(phys);
    if (!rec) return;
    if (!rec->in_use) return;  /* silent on double-free */
    rec->in_use = 0;
    brk_state.total_frees++;
}

/* ── PTE table helpers ───────────────────────────────────── */
int brk_pte_index_for(uint64_t va)
{
    if (va < BRK_PTE_USER_OFFSET) return -1;
    uint64_t off = va - BRK_PTE_USER_OFFSET;
    if (off >= (uint64_t)BRK_PTE_TABLE_SIZE * 0x1000UL) return -1;
    return (int)(off >> 12);
}

brk_pte_record_t *brk_find_mapping(uint64_t va)
{
    int idx = brk_pte_index_for(va);
    if (idx < 0) return NULL;
    if (!(brk_state.ptes[idx].pte & PAGE_VALID)) return NULL;
    return &brk_state.ptes[idx];
}

/* ── COW refcount helpers ──────────────────────────────────
 * page_cow_refs reads the per-phys counter; get/put put in/decrement.
 * Production semantics: page_cow_put returns true when the count
 * reaches zero (caller typically frees the phys in that case).
 *
 * Our vmm_unmap_4k_page uses page_cow_put like production:
 *   if (page_cow_put(phys)) free_4k_page(phys);
 * so the count must transition through 0 → true for the phys to
 * be released. */
void page_cow_get(uint64_t phys)
{
    brk_page_pool_init();
    brk_page_record_t *rec = brk_find_page(phys);
    if (!rec) return;
    rec->cow_refs++;
    brk_state.total_cow_gets++;
}

bool page_cow_put(uint64_t phys)
{
    brk_page_pool_init();
    brk_page_record_t *rec = brk_find_page(phys);
    if (!rec) return true;
    if (rec->cow_refs > 0) rec->cow_refs--;
    if (rec->cow_refs == 0) {
        brk_state.total_cow_puts++;
        return true;     /* production returns true on zero */
    }
    return false;
}

uint16_t page_cow_refs(uint64_t phys)
{
    brk_page_pool_init();
    brk_page_record_t *rec = brk_find_page(phys);
    if (!rec) return 0;
    return rec->cow_refs;
}

/* ── vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page ────
 * Flat indexed PTE table — no intermediate page-table levels,
 * no PAGE_HUGE leaves.  Brk must never touch a 2 MiB leaf.
 *
 * alloc_4k_page must be called before map so the phys is recorded.
 * The test verifies this contract (no "map-without-alloc"). */
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)flags;
    int idx = brk_pte_index_for(virt);
    if (idx < 0) return NULL;
    return &brk_state.ptes[idx].pte;
}

int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    (void)pgdir;

    /* Honour failure injection: AFTER N successful maps, the next
     * call returns -1. */
    if (brk_state.inject_map_fail_at > 0 &&
        brk_state.total_maps + 1 == brk_state.inject_map_fail_at) {
        return -1;
    }

    int idx = brk_pte_index_for(virt);
    if (idx < 0) return -1;
    if (phys & ~PAGE_4K_MASK) return -1;   /* mis-aligned */
    brk_pte_record_t *rec = &brk_state.ptes[idx];
    rec->pte   = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    rec->phys  = phys;
    rec->flags = flags;
    brk_state.total_maps++;
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    int idx = brk_pte_index_for(virt);
    if (idx < 0) return;
    brk_pte_record_t *rec = &brk_state.ptes[idx];

    /* Must check both Valid and PROTNONE -- PROTNONE pages have
     * Valid=0 but valid phys that must be freed. */
    if (!(rec->pte & (PAGE_VALID | PAGE_PROTNONE)))
        return;

    uint64_t phys = rec->pte & PAGE_4K_MASK;

    if (rec->pte & PAGE_COW) {
        /* COW-shared page: decrement refcount, free only when count hits 0 */
        if (page_cow_put(phys))
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    rec->pte   = 0;
    rec->phys  = 0;
    rec->flags = 0;
    brk_state.total_unmaps++;
}