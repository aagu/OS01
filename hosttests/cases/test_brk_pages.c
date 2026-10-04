/*
 * test/cases/test_brk_pages.c — Host tests for the brk-page
 * ownership contract (Task 4, user heap/ELF isolation plan).
 *
 * Strategy C (Hybrid) — production-linked mm_set_brk via
 * hosttests/mock/brk/ + source-level inspection of trap.c.
 *
 * Production-linked coverage compiles the REAL
 * kernel/memory/vma.c against a flat indexed PTE table that
 * supports PAGE_VALID + PAGE_COW + PAGE_PROTNONE flag bits.
 * mm_set_brk is exercised against observable state — every
 * alloc/free/map/unmap/COW-decrement is counted, every flush_tlb
 * call is recorded.
 *
 * Source-level inspection of kernel/arch/x86_64/intr/trap.c covers:
 *   - SYS_brk delegates to mm_set_brk and returns the right value;
 *   - do_page_fault heap path may privately resolve an already-
 *     mapped COW leaf but must NOT demand-map an absent heap leaf.
 *
 * Brief Step 1 coverage (test list below mirrors the brief
 * checklist — query / lower / upper bound / same-page growth /
 * multi-page zeroed growth / OOM rollback / shrink-with-COW /
 * partial-tail COW privatization / regrowth / zeroed-on-regrow).
 */
#include "test_framework.h"
#include "brk_stubs.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <fs/vfs.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)

/* Forward-declare mm_set_brk so the test builds RED before the
 * production declaration lands in kernel/include/memory/vma.h.
 * Once the production header gains the prototype, this forward
 * declaration is harmless (declarations match). */
int mm_set_brk(mm_t *mm, uint64_t requested, uint64_t *result);

/* ── Layout constants (mirror kernel/include/sched/task.h) ── */
#define USER_CODE_ADDR  0x400000UL
#define USER_PAGE_SIZE  0x1000000UL
#define USER_STACK_BASE 0x1400000UL
#define HEAP_LIMIT      (USER_CODE_ADDR + USER_PAGE_SIZE - 0x1000UL)

#define PAGE_VALID_BIT  0x1UL
#define PAGE_WRITE_BIT  0x2UL
#define PAGE_USER_BIT   0x4UL
#define PAGE_COW_BIT    (1UL << 10)
#define PAGE_PROTNONE_BIT (1UL << 9)

/* Production wraps (--wrap=kmalloc,kfree) for OOM injection. */
extern void *__real_kmalloc(size_t size);
extern void  __real_kfree(void *ptr);

static int kmalloc_calls = 0;
static int kmalloc_fail_after = 0;  /* 0 = never fail */

void *__wrap_kmalloc(size_t size)
{
    kmalloc_calls++;
    if (kmalloc_fail_after > 0 && kmalloc_calls == kmalloc_fail_after)
        return NULL;
    return __real_kmalloc(size);
}

void __wrap_kfree(void *ptr)
{
    __real_kfree(ptr);
}

/* ── Per-test fixture ─────────────────────────────────────── */

static mm_t fixture_mm;
static vma_t *heap_vma;

static void setup_mm(uint64_t elf_end)
{
    brk_stubs_reset();
    kmalloc_calls = 0;
    kmalloc_fail_after = 0;
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    /* Identity pgdir: any non-NULL pointer (the host walker uses
     * identity Phy_To_Virt). */
    fixture_mm.pgdir = (uint64_t *)0x100000ULL;
    fixture_mm.mmap_base = 0x40000000UL;
    /* Run the real heap initializer */
    int rc = mm_init_user_heap(&fixture_mm, elf_end);
    assert_eq(0, rc);

    /* Find the heap VMA.  vma_find won't match it (zero-length),
     * so we walk the list directly. */
    heap_vma = NULL;
    for (list_t *p = fixture_mm.vma_list.next;
         p != &fixture_mm.vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_flags & VM_HEAP) { heap_vma = v; break; }
    }
    assert_not_null(heap_vma);
}

/* ── Helpers ──────────────────────────────────────────────── */

static int vma_list_count(list_t *head) __attribute__((unused));
static int vma_list_count(list_t *head)
{
    int n = 0;
    for (list_t *p = head->next; p != head; p = p->next) n++;
    return n;
}

/* Count consecutive mapped pages starting at va, page-step. */
static int count_mapped_pages(uint64_t start_va, int max)
{
    int n = 0;
    for (uint64_t va = start_va; n < max; va += PAGE_4K_SIZE, n++) {
        if (!brk_find_mapping(va)) break;
    }
    return n;
}

/* ── Brief Step 1: mm_set_brk tests ──────────────────────── */

static void test_brk_query_returns_current(void)
{
    TEST_SUITE("mm_set_brk — query returns current end_brk");

    setup_mm(USER_CODE_ADDR + 0x1000);

    uint64_t result = 0xDEADBEEF;
    int ret = mm_set_brk(&fixture_mm, 0, &result);
    assert_eq(0, ret);
    assert_eq(fixture_mm.start_brk, result);
    /* Query must NOT change end_brk, vm_end, or PTEs. */
    assert_eq(fixture_mm.start_brk, fixture_mm.end_brk);
    assert_eq(heap_vma->vm_end, fixture_mm.end_brk);
    assert_eq(0, brk_state.total_maps);
}

static void test_brk_query_with_null_result_pointer(void)
{
    TEST_SUITE("mm_set_brk — query accepts NULL result pointer");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Per the brief, requested == 0 returns 0 (success). The
     * kernel-side SYS_brk handler always passes a real &result,
     * but mm_set_brk itself must not panic on NULL result. */
    int ret = mm_set_brk(&fixture_mm, 0, NULL);
    assert_eq(0, ret);
}

static void test_brk_lower_bound_returns_einval(void)
{
    TEST_SUITE("mm_set_brk — requested < start_brk → -EINVAL");

    setup_mm(USER_CODE_ADDR + 0x1000);

    uint64_t result = 0xDEADBEEF;
    uint64_t requested = fixture_mm.start_brk - 1;
    int ret = mm_set_brk(&fixture_mm, requested, &result);
    assert_eq(-EINVAL, ret);
    /* result must be unchanged on failure. */
    assert_eq((uint64_t)0xDEADBEEF, result);
    /* End break + VMA + PTEs unchanged. */
    assert_eq(fixture_mm.start_brk, fixture_mm.end_brk);
    assert_eq(heap_vma->vm_end, fixture_mm.end_brk);
    assert_eq(0, brk_state.total_maps);
}

static void test_brk_upper_bound_returns_enomem(void)
{
    TEST_SUITE("mm_set_brk — requested > heap_limit → -ENOMEM");

    setup_mm(USER_CODE_ADDR + 0x1000);

    uint64_t result = 0xDEADBEEF;
    uint64_t requested = HEAP_LIMIT + 1;
    int ret = mm_set_brk(&fixture_mm, requested, &result);
    assert_eq(-ENOMEM, ret);
    assert_eq((uint64_t)0xDEADBEEF, result);
    assert_eq(fixture_mm.start_brk, fixture_mm.end_brk);
    assert_eq(0, brk_state.total_maps);
}

static void test_brk_same_page_growth_no_new_pte(void)
{
    TEST_SUITE("mm_set_brk — same-page growth (no new PTE)");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* First grow by 0x100 within the first page (one 4 KiB leaf
     * mapped, end_brk ends partway through that page).  Then
     * grow by another 0x100 within the SAME page — must NOT map
     * a new PTE (brief §5.2: "同页内增长无需新页"). */
    uint64_t partial = fixture_mm.start_brk + 0x100;
    uint64_t result;
    int ret = mm_set_brk(&fixture_mm, partial, &result);
    assert_eq(0, ret);
    assert_eq(1, brk_state.total_maps);    /* baseline (first page) */

    uint64_t new_brk = partial + 0x100;     /* still in the same page */
    int maps_before = brk_state.total_maps;
    int allocs_before = brk_state.total_allocs;
    ret = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, ret);
    assert_eq(new_brk, result);
    assert_eq(new_brk, fixture_mm.end_brk);
    assert_eq(new_brk, heap_vma->vm_end);
    assert_eq(maps_before,   brk_state.total_maps);
    assert_eq(allocs_before, brk_state.total_allocs);
}

static void test_brk_multipage_growth_each_page_zeroed(void)
{
    TEST_SUITE("mm_set_brk — multi-page growth (each page zeroed)");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Move break forward by 3 pages. */
    uint64_t new_brk = fixture_mm.start_brk + 3 * PAGE_4K_SIZE;
    uint64_t result;
    int ret = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, ret);
    assert_eq(new_brk, result);

    /* All 3 pages mapped, none COW. */
    assert_eq(3, count_mapped_pages(fixture_mm.start_brk, 4));
    assert_eq(3, brk_state.total_maps);
    assert_eq(3, brk_state.total_allocs);

    /* Each page must be zeroed: every backing buffer at those
     * phys addrs is all-zero. */
    for (int i = 0; i < 3; i++) {
        uint64_t va = fixture_mm.start_brk + i * PAGE_4K_SIZE;
        brk_pte_record_t *m = brk_find_mapping(va);
        assert_not_null(m);
        brk_page_record_t *rec = brk_find_page(m->phys);
        assert_not_null(rec);
        unsigned char *p = (unsigned char *)rec->backing;
        for (int j = 0; j < 64; j++) {   /* spot-check first 64 bytes */
            assert_eq(0, (int)p[j]);
        }
    }
    /* PTE flags: user R/W present. */
    brk_pte_record_t *m = brk_find_mapping(fixture_mm.start_brk);
    assert_eq(PAGE_USER | PAGE_WRITE | PAGE_VALID, m->flags);
}

static void test_brk_multipage_growth_flushes_tlb(void)
{
    TEST_SUITE("mm_set_brk — multi-page growth flushes TLB");

    setup_mm(USER_CODE_ADDR + 0x1000);

    int tlb_before = brk_state.total_tlb_shootdowns;
    uint64_t new_brk = fixture_mm.start_brk + 2 * PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);
    /* At least one TLB flush after multi-page growth. */
    assert_true(brk_state.total_tlb_shootdowns > tlb_before);
}

static void test_brk_oom_on_second_page_rolls_back(void)
{
    TEST_SUITE("mm_set_brk — OOM on second new page → no commit");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Inject OOM at the second alloc_4k_page (we want the FIRST
     * page to be staged successfully and the second to fail —
     * this proves rollback frees the first page too). */
    brk_state.inject_alloc_fail_at = 2;

    int saved_maps = brk_state.total_maps;
    (void)saved_maps;
    int saved_allocs = brk_state.total_allocs;
    int saved_frees = brk_state.total_frees;

    uint64_t new_brk = fixture_mm.start_brk + 3 * PAGE_4K_SIZE;
    uint64_t result = 0xDEADBEEF;
    int rc = mm_set_brk(&fixture_mm, new_brk, &result);

    /* Must fail with -ENOMEM, result unchanged. */
    assert_eq(-ENOMEM, rc);
    assert_eq((uint64_t)0xDEADBEEF, result);

    /* End break / VMA end / start_brk all unchanged. */
    assert_eq(fixture_mm.start_brk, fixture_mm.end_brk);
    assert_eq(heap_vma->vm_end, fixture_mm.end_brk);

    /* PTE state: no new page was committed.  The first alloc
     * succeeded (counter advanced), then the second failed, and
     * the rollback path unmapped it.  Net allocs == 1, net frees
     * == 1 (the rolled-back first page). */
    assert_eq(saved_allocs + 1, brk_state.total_allocs);
    assert_eq(saved_frees  + 1, brk_state.total_frees);
    /* No PTE entries in the [start_brk, new_brk) range. */
    assert_eq(0, count_mapped_pages(fixture_mm.start_brk, 4));
}

static void test_brk_shrink_across_page_releases_leaves(void)
{
    TEST_SUITE("mm_set_brk — shrink across a page releases leaves + flushes TLB");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* First grow by 4 pages. */
    uint64_t old_brk = fixture_mm.start_brk + 4 * PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, old_brk, &result);
    assert_eq(0, rc);
    assert_eq(4, count_mapped_pages(fixture_mm.start_brk, 8));

    /* Now shrink by 2 pages (release the last 2 mapped leaves). */
    int tlb_before = brk_state.total_tlb_shootdowns;
    uint64_t new_brk = old_brk - 2 * PAGE_4K_SIZE;
    rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);
    assert_eq(new_brk, result);
    assert_eq(new_brk, fixture_mm.end_brk);
    assert_eq(new_brk, heap_vma->vm_end);

    /* 2 pages remain mapped; the last 2 are unmapped. */
    assert_eq(2, count_mapped_pages(fixture_mm.start_brk, 8));
    /* 2 unmaps + 2 frees happened. */
    assert_eq(2, brk_state.total_unmaps);
    assert_eq(2, brk_state.total_frees);
    /* TLB flushed at least once after the shrink. */
    assert_true(brk_state.total_tlb_shootdowns > tlb_before);
}

static void test_brk_shrink_partial_tail_zeros(void)
{
    TEST_SUITE("mm_set_brk — partial-tail shrink zeroes retained bytes");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Grow by 2 pages and write into the second page so we can
     * observe the partial-tail zeroing. */
    uint64_t old_brk = fixture_mm.start_brk + 2 * PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, old_brk, &result);
    assert_eq(0, rc);

    /* Fill the second page with a recognizable pattern. */
    brk_pte_record_t *m = brk_find_mapping(fixture_mm.start_brk + PAGE_4K_SIZE);
    assert_not_null(m);
    brk_page_record_t *rec = brk_find_page(m->phys);
    assert_not_null(rec);
    memset(rec->backing, 'X', 4096);

    /* Shrink by 0x100 bytes within the second page. */
    uint64_t new_brk = old_brk - 0x100;
    rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);
    assert_eq(new_brk, result);

    /* The retained page is still mapped (no shrink across pages). */
    brk_pte_record_t *m2 = brk_find_mapping(fixture_mm.start_brk + PAGE_4K_SIZE);
    assert_not_null(m2);

    /* Bytes [new_brk - start_brk, PAGE_4K_SIZE) within the page
     * must be zero. */
    size_t retain_off = (size_t)(new_brk - (fixture_mm.start_brk + PAGE_4K_SIZE));
    assert_eq(PAGE_4K_SIZE - 0x100, retain_off);
    unsigned char *p = (unsigned char *)rec->backing;
    for (size_t i = retain_off; i < 4096; i++) {
        assert_eq(0, (int)p[i]);
    }
    /* Bytes [0, retain_off) must still be 'X' (untouched). */
    for (size_t i = 0; i < retain_off; i++) {
        assert_eq('X', (int)p[i]);
    }
}

static void test_brk_shrink_partial_tail_privatizes_cow(void)
{
    TEST_SUITE("mm_set_brk — partial-tail shrink privatizes COW tail");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Grow by 1 page, then mark that page as COW with refs > 1
     * so the shrink's retained-tail privatization kicks in. */
    uint64_t old_brk = fixture_mm.start_brk + PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, old_brk, &result);
    assert_eq(0, rc);

    brk_pte_record_t *m = brk_find_mapping(fixture_mm.start_brk);
    assert_not_null(m);
    /* Set PAGE_COW in the PTE and bump the per-phys COW refs to 2. */
    m->pte = (m->pte & ~PAGE_WRITE) | PAGE_COW;
    m->flags = (m->flags & ~PAGE_WRITE) | PAGE_COW;
    brk_page_record_t *rec = brk_find_page(m->phys);
    assert_not_null(rec);
    rec->cow_refs = 2;     /* simulate a sharer */

    int allocs_before = brk_state.total_allocs;
    int frees_before  = brk_state.total_frees;
    int cow_puts_before = brk_state.total_cow_puts;

    /* Shrink by 0x100 — within the page.  Must privatize. */
    uint64_t new_brk = old_brk - 0x100;
    rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);

    /* The PTE now points to a NEW phys (privatization).  The PTE
     * holds the canonical phys (low bits masked off); the standalone
     * .phys field on the test record is not updated by mm_set_brk's
     * in-place PTE write (production doesn't know about it). */
    brk_pte_record_t *m2 = brk_find_mapping(fixture_mm.start_brk);
    assert_not_null(m2);
    uint64_t new_phys_from_pte = m2->pte & PAGE_4K_MASK;
    assert_true(new_phys_from_pte != m->phys);    /* phys changed */

    /* The OLD phys is still allocated — page_cow_put dropped
     * its cow_refs from 2 → 1 (still shared with a sibling
     * process), so free_4k_page was NOT called for the old
     * phys.  The brief path: privatize → alloc new → copy →
     * replace PTE → page_cow_put(old).  After put, refs=1, the
     * phys stays allocated (the last sharer will free it). */
    assert_eq(1, rec->in_use);   /* still allocated */
    assert_eq(1, rec->cow_refs); /* decremented to 1 */

    /* Exactly one new alloc (the private copy). */
    assert_eq(allocs_before + 1, brk_state.total_allocs);
    assert_eq(frees_before,      brk_state.total_frees);
    assert_eq(cow_puts_before,   brk_state.total_cow_puts);  /* put didn't reach 0 */

    /* The PTE no longer has PAGE_COW set (private leaf). */
    assert_eq(0, (int)(m2->pte & PAGE_COW_BIT));

    /* The OLD phys is still allocated (cow_refs=2 → 1, but still
     * shared with a sibling — never freed in this shrink). */
    assert_eq(1, rec->in_use);
    assert_eq(1, rec->cow_refs);
}

static void test_brk_shrink_release_cow_last_ref_frees(void)
{
    TEST_SUITE("mm_set_brk — shrink releasing a COW leaf drops COW ref");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Grow by 2 pages; mark both as COW with refs=2. */
    uint64_t old_brk = fixture_mm.start_brk + 2 * PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, old_brk, &result);
    assert_eq(0, rc);

    brk_pte_record_t *m1 = brk_find_mapping(fixture_mm.start_brk);
    brk_pte_record_t *m2 = brk_find_mapping(fixture_mm.start_brk + PAGE_4K_SIZE);
    assert_not_null(m1);
    assert_not_null(m2);
    m1->pte = (m1->pte & ~PAGE_WRITE) | PAGE_COW;
    m1->flags = (m1->flags & ~PAGE_WRITE) | PAGE_COW;
    m2->pte = (m2->pte & ~PAGE_WRITE) | PAGE_COW;
    m2->flags = (m2->flags & ~PAGE_WRITE) | PAGE_COW;
    brk_page_record_t *r1 = brk_find_page(m1->phys);
    brk_page_record_t *r2 = brk_find_page(m2->phys);
    assert_not_null(r1);
    assert_not_null(r2);
    r1->cow_refs = 2;
    r2->cow_refs = 2;

    /* Shrink by 2 pages → both leaves unmapped.  vmm_unmap_4k_page
     * drops the COW ref to 1 each, so neither phys is freed yet
     * (still shared with a sibling). */
    uint64_t new_brk = fixture_mm.start_brk;
    rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);

    assert_eq(1, r1->cow_refs);
    assert_eq(1, r2->cow_refs);
    assert_eq(1, r1->in_use);
    assert_eq(1, r2->in_use);
    assert_eq(2, brk_state.total_unmaps);
    assert_eq(0, brk_state.total_frees);   /* neither freed (refs=1) */
}

static void test_brk_regrowth_returns_zeroed_pages(void)
{
    TEST_SUITE("mm_set_brk — regrowth after shrink returns zeroed pages");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Grow by 2 pages, dirty the second page, shrink by 2, then
     * regrow by 2 and verify the second page is zeroed (no leak
     * of stale data). */
    uint64_t grown = fixture_mm.start_brk + 2 * PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, grown, &result);
    assert_eq(0, rc);

    /* Fill the second page with non-zero data. */
    brk_pte_record_t *m = brk_find_mapping(fixture_mm.start_brk + PAGE_4K_SIZE);
    assert_not_null(m);
    brk_page_record_t *rec = brk_find_page(m->phys);
    memset(rec->backing, 'A', 4096);

    /* Shrink back to start_brk. */
    rc = mm_set_brk(&fixture_mm, fixture_mm.start_brk, &result);
    assert_eq(0, rc);
    assert_eq(0, count_mapped_pages(fixture_mm.start_brk, 4));

    /* Regrow by 2 pages. */
    rc = mm_set_brk(&fixture_mm, grown, &result);
    assert_eq(0, rc);
    assert_eq(2, count_mapped_pages(fixture_mm.start_brk, 4));

    /* Both pages are zeroed — no leak of 'A' across shrink/regrow. */
    brk_pte_record_t *m1 = brk_find_mapping(fixture_mm.start_brk);
    brk_pte_record_t *m2 = brk_find_mapping(fixture_mm.start_brk + PAGE_4K_SIZE);
    assert_not_null(m1);
    assert_not_null(m2);
    brk_page_record_t *rec1 = brk_find_page(m1->phys);
    brk_page_record_t *rec2 = brk_find_page(m2->phys);
    assert_not_null(rec1);
    assert_not_null(rec2);
    unsigned char *p1 = (unsigned char *)rec1->backing;
    unsigned char *p2 = (unsigned char *)rec2->backing;
    for (int j = 0; j < 64; j++) {
        assert_eq(0, (int)p1[j]);
        assert_eq(0, (int)p2[j]);
    }
}

static void test_brk_grow_then_shrink_preserves_cow_refs(void)
{
    TEST_SUITE("mm_set_brk — grow then shrink: COW refs on surviving leaves untouched");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Grow by 2 pages; only mark the FIRST as COW. */
    uint64_t grown = fixture_mm.start_brk + 2 * PAGE_4K_SIZE;
    uint64_t result;
    int rc = mm_set_brk(&fixture_mm, grown, &result);
    assert_eq(0, rc);

    brk_pte_record_t *m1 = brk_find_mapping(fixture_mm.start_brk);
    m1->pte = (m1->pte & ~PAGE_WRITE) | PAGE_COW;
    m1->flags = (m1->flags & ~PAGE_WRITE) | PAGE_COW;
    brk_page_record_t *r1 = brk_find_page(m1->phys);
    r1->cow_refs = 2;

    /* Shrink by exactly 1 page — the second (non-COW) page is
     * unmapped.  The first (COW) leaf must be unchanged. */
    uint64_t new_brk = fixture_mm.start_brk + PAGE_4K_SIZE;
    rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);

    brk_pte_record_t *m1_after = brk_find_mapping(fixture_mm.start_brk);
    assert_not_null(m1_after);
    assert_eq(m1->phys, m1_after->phys);    /* unchanged phys */
    assert_eq(2, r1->cow_refs);             /* unchanged COW refs */
}

static void test_brk_result_set_on_success(void)
{
    TEST_SUITE("mm_set_brk — result reflects new end_brk on success");

    setup_mm(USER_CODE_ADDR + 0x1000);

    uint64_t new_brk = fixture_mm.start_brk + 0x500;  /* unaligned */
    uint64_t result = 0xDEADBEEF;
    int rc = mm_set_brk(&fixture_mm, new_brk, &result);
    assert_eq(0, rc);
    assert_eq(new_brk, result);
    assert_eq(new_brk, fixture_mm.end_brk);
    /* heap_vma->vm_end tracks end_brk per the brief. */
    assert_eq(new_brk, heap_vma->vm_end);
}

/* ── Source-level inspection: SYS_brk delegation ───────────
 *
 * trap.c is too heavy to host-link (2200-line file with arch
 * dependency tree).  The brief accepts source-level inspection
 * for the syscall delegation; the end-to-end behavior is
 * covered by `make OS01_SYSTEST=1 test-qemu SUITE=systest`.
 */

/* Locate kernel/arch/x86_64/intr/trap.c relative to THIS test file
 * (__FILE__), never a hardcoded absolute path: the suite must pass in
 * any checkout/worktree/CI.  __FILE__ is derived from how the Makefile
 * compiles this TU (TEST_CASES is $(realpath ..)/hosttests/cases, so
 * it is absolute and contains "/hosttests/").  Fallbacks cover a
 * relative __FILE__ (resolve via getcwd()) and a cwd inside the
 * hosttests directory.  Returns NULL if the layout is unrecognized. */
static const char *trap_c_path(void)
{
    static char buf[1024];
    char full[1024];
    const char *f = __FILE__;
    const char *marker;

    if (f[0] == '/') {
        snprintf(full, sizeof(full), "%s", f);
    } else {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
        snprintf(full, sizeof(full), "%s/%s", cwd, f);
    }

    marker = strstr(full, "/hosttests/");
    if (marker) {
        snprintf(buf, sizeof(buf),
                 "%.*s/kernel/arch/x86_64/intr/trap.c",
                 (int)(marker - full), full);
        return buf;
    }
    /* Relative __FILE__ ("hosttests/cases/...") with cwd == repo root:
     * the repo root is the current directory. */
    if (strncmp(full, "hosttests/", 10) == 0)
        return "kernel/arch/x86_64/intr/trap.c";
    return NULL;
}

#define TRAP_C_BUDGET (8 * 1024)

static char *slurp_file(const char *path, size_t *out_len)
{
    int fd = open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        int64_t got = read(fd, buf + len, 4096);
        if (got < 0) { free(buf); close(fd); return NULL; }
        if (got == 0) break;
        len += (size_t)got;
    }
    close(fd);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static int source_contains(char *haystack, size_t haystack_len,
                           const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, n) == 0) return 1;
    }
    return 0;
}

static int source_contains_in_case(char *haystack, size_t haystack_len,
                                   const char *case_label, const char *needle)
{
    char *p = strstr(haystack, case_label);
    if (!p) return 0;
    char *scan_limit = p + TRAP_C_BUDGET;
    if (scan_limit > haystack + haystack_len) scan_limit = haystack + haystack_len;
    return source_contains(p, (size_t)(scan_limit - p), needle);
}

/* Bounded forward search: first occurrence of `needle` at or after
 * `start`, no further than `max_off` bytes away.  NULL if absent. */
static char *find_within(const char *start, size_t max_off,
                         const char *needle)
{
    size_t n = strlen(needle);
    const char *end = start + max_off;
    for (const char *p = start; p + n <= end; p++) {
        if (memcmp(p, needle, n) == 0) return (char *)p;
    }
    return NULL;
}

/* True if the source line containing `at` is ACTIVE code, i.e. not a
 * preprocessor line and not a comment line (leading non-space
 * characters are checked against the comment introducers). */
static int line_is_active_code(const char *src, const char *at)
{
    const char *line = at;
    while (line > src && line[-1] != '\n') line--;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '/' || *p == '*') return 0;
    return 1;
}

static void test_sys_brk_calls_mm_set_brk(void)
{
    TEST_SUITE("SYS_brk — delegates to mm_set_brk (old direct writeback absent)");

    size_t len;
    char *src = slurp_file(trap_c_path(), &len);
    assert_not_null(src);

    /* Bound the case body by the next case label so the checks cannot
     * be satisfied by code elsewhere in the dispatcher. */
    char *brk_case = strstr(src, "case SYS_brk: {");
    assert_not_null(brk_case);
    char *brk_end = strstr(brk_case, "case SYS_getpid");
    assert_not_null(brk_end);
    size_t body_len = (size_t)(brk_end - brk_case);

    /* The SYS_brk case body must call mm_set_brk(mm, addr, &result). */
    assert_true(source_contains(brk_case, body_len, "mm_set_brk("));

    /* Smoking gun for the old (pre-Task-4) handler: the direct
     * `mm->end_brk = addr` writeback.  Delegation must have replaced
     * it entirely. */
    assert_true(!source_contains(brk_case, body_len, "mm->end_brk = addr"));

    /* Success returns *result, not the raw requested address. */
    assert_true(source_contains(brk_case, body_len, "regs->rax = result"));

    /* Must include <memory/vma.h> so the prototype is visible. */
    assert_true(source_contains(src, len, "#include <memory/vma.h>"));

    free(src);
}

static void test_sys_brk_returns_result_on_success(void)
{
    TEST_SUITE("SYS_brk — returns *result on success");

    size_t len;
    char *src = slurp_file(trap_c_path(), &len);
    assert_not_null(src);

    /* SYS_brk must store the result value (not raw end_brk) so
     * that the mm_set_brk contract — "on failure *result unchanged" —
     * holds.  Pattern: regs->rax = result; (after a successful
     * mm_set_brk call). */
    int has_pattern = source_contains_in_case(
        src, len,
        "case SYS_brk: {",
        "regs->rax = result;");
    assert_true(has_pattern);

    free(src);
}

static void test_heap_fault_does_not_demand_map(void)
{
    TEST_SUITE("do_page_fault — heap absent-leaf guard ACTIVE, precedes demand mapping");

    size_t len;
    char *src = slurp_file(trap_c_path(), &len);
    assert_not_null(src);

    /* Locate the P=0 demand-allocation block inside do_page_fault. */
    char *fn = strstr(src, "void do_page_fault(pt_regs_t *");
    assert_not_null(fn);
    char *demand = strstr(fn, "if (!(error_code & 0x01)) {");
    assert_not_null(demand);

    size_t tail = (size_t)(src + len - demand);
    char *first_map = find_within(demand, tail, "alloc_4k_page");
    assert_not_null(first_map);   /* the demand block does map pages */

    /* Contract 1: an explicit VM_HEAP guard sits between the start of
     * the demand block and the first demand-mapping attempt. */
    char *guard = find_within(demand, (size_t)(first_map - demand), "VM_HEAP");
    assert_not_null(guard);

    /* Contract 2: the guard is ACTIVE code — the in-flight regression
     * was `#if 0`-disabling this exact block, whose text still
     * matched plain string searches. */
    assert_true(find_within(demand, (size_t)(guard - demand), "#if 0") == NULL);
    assert_true(line_is_active_code(src, guard));

    /* Contract 3: the guard kills before it can fall through —
     * kill_current_user_task then return, both ahead of first_map. */
    char *kill = find_within(guard, (size_t)(first_map - guard),
                             "kill_current_user_task");
    assert_not_null(kill);
    char *ret = find_within(kill, (size_t)(first_map - kill), "return");
    assert_not_null(ret);

    free(src);
}

static void test_heap_fault_resolves_cow_for_committed_range(void)
{
    TEST_SUITE("do_page_fault — heap VMA COW leaf resolves under mm->lock");

    size_t len;
    char *src = slurp_file(trap_c_path(), &len);
    assert_not_null(src);

    /* The COW resolution path (PAGE_COW check) must still be
     * present — heap VMA faults on an ALREADY-MAPPED COW leaf
     * must be privately resolved. */
    char *fn = strstr(src, "void do_page_fault(pt_regs_t *");
    assert_not_null(fn);
    char *fn_end = fn + 16 * 1024;
    if (fn_end > src + len) fn_end = src + len;
    size_t span = (size_t)(fn_end - fn);

    int has_cow_branch = 0;
    for (size_t i = 0; i + strlen("PAGE_COW") <= span; i++) {
        if (memcmp(fn + i, "PAGE_COW", strlen("PAGE_COW")) == 0) {
            has_cow_branch = 1;
            break;
        }
    }
    assert_true(has_cow_branch);

    free(src);
}

/* ── Test list ─────────────────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_brk_query_returns_current),
    TEST_ENTRY(test_brk_query_with_null_result_pointer),
    TEST_ENTRY(test_brk_lower_bound_returns_einval),
    TEST_ENTRY(test_brk_upper_bound_returns_enomem),
    TEST_ENTRY(test_brk_same_page_growth_no_new_pte),
    TEST_ENTRY(test_brk_multipage_growth_each_page_zeroed),
    TEST_ENTRY(test_brk_multipage_growth_flushes_tlb),
    TEST_ENTRY(test_brk_oom_on_second_page_rolls_back),
    TEST_ENTRY(test_brk_shrink_across_page_releases_leaves),
    TEST_ENTRY(test_brk_shrink_partial_tail_zeros),
    TEST_ENTRY(test_brk_shrink_partial_tail_privatizes_cow),
    TEST_ENTRY(test_brk_shrink_release_cow_last_ref_frees),
    TEST_ENTRY(test_brk_regrowth_returns_zeroed_pages),
    TEST_ENTRY(test_brk_grow_then_shrink_preserves_cow_refs),
    TEST_ENTRY(test_brk_result_set_on_success),
    TEST_ENTRY(test_sys_brk_calls_mm_set_brk),
    TEST_ENTRY(test_sys_brk_returns_result_on_success),
    TEST_ENTRY(test_heap_fault_does_not_demand_map),
    TEST_ENTRY(test_heap_fault_resolves_cow_for_committed_range),
TEST_LIST_END

/* NOTE: we intentionally do NOT call setbuf(stdout, NULL) — the
 * OS01 libc stub defines stdout as ((FILE*)2), which the host
 * libc's setbuf dereferences and SIGSEGVs.  Instead we use
 * fflush(stdout) after each TEST_SUITE boundary in the test
 * functions themselves; the [PASS]/[FAIL] prints come straight
 * through to the terminal unbuffered by default. */

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed == 0 ? 0 : 1;
}