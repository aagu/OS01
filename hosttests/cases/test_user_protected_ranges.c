/*
 * test/cases/test_user_protected_ranges.c — Host tests for the
 * reserved-window predicate mm_user_range_protected and its wiring
 * into do_mmap / do_munmap_locked / do_mprotect (Task 5, user
 * heap/ELF isolation plan).
 *
 * Protected ranges (page-aligned, half-open):
 *   [0x400000,        mm->start_brk)   ELF reserve envelope
 *                                      (includes inter-segment gaps)
 *   [mm->start_brk,   0x13ff000)       heap reserve (committed +
 *                                      uncommitted window; covers the
 *                                      zero-length heap VMA's range)
 *   [0x13ff000,       0x1400000)       heap→stack guard page
 *   [0x1400000,       0x1600000)       user stack (2 MiB)
 *
 * PRODUCTION-LINKED: compiles the REAL kernel/memory/vma.c against
 * hosttests/mock/pranges/ (flat indexed PTE table, 4 KiB page pool,
 * counter-backed TLB flush).  do_mmap / do_munmap / do_mprotect are
 * driven through the harness's `current`; every alloc/free/map/
 * unmap is counted so the "no partial mutation" contract is
 * observable.
 *
 * The device-mmap callback itself is NOT reachable on the host —
 * do_mmap gates it on `ops >= 0xffff800000000000` (kernel-half
 * heuristic), which no host-heap pointer satisfies.  The
 * "rejected request must skip the device callback" contract is
 * therefore covered by SOURCE-LEVEL INSPECTION of
 * kernel/memory/vma.c (same strategy the brk/lifecycle tests use
 * for trap.c/task.c), asserting the predicate check textually
 * precedes the _dev_mmap invocation, the MAP_FIXED
 * do_munmap_locked call, the first VMA-flag write in do_mprotect
 * and the first PTE unmap in do_munmap_locked.
 */
#include "test_framework.h"
#include "pranges_stubs.h"

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

/* Forward declaration — before Task 5 lands this lives only as the
 * weak RED fallback in pranges_stubs.c (returns false); afterwards
 * the production prototype in <memory/vma.h> matches it exactly. */
bool mm_user_range_protected(const mm_t *mm, uint64_t start, uint64_t end);

/* ── Layout constants (mirror kernel/memory/vma.c) ─────────── */
#define USER_CODE_ADDR     0x400000UL
#define USER_ENVELOPE_SIZE 0x20000000UL
#define USER_PAGE_SIZE     USER_ENVELOPE_SIZE
#define USER_STACK_BASE    0x20400000UL
#define HEAP_LIMIT         (USER_CODE_ADDR + USER_ENVELOPE_SIZE - 0x1000UL)
#define USER_STACK_END     (USER_STACK_BASE + 0x200000UL)
#define PROD_ADDR_LIMIT  0xffff800000000000UL

#define PROT_RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_ANONYMOUS | MAP_PRIVATE)

/* ── Per-test fixture ───────────────────────────────────────── */

static mm_t fixture_mm;

/* setup_mm: fresh stub state + user heap initialised at elf_end.
 * `current` points at the fixture with a production-shaped
 * addr_limit, so do_mmap/do_munmap/do_mprotect run for real. */
static void setup_mm(uint64_t elf_end)
{
    pr_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = (uint64_t *)0x100000ULL;  /* identity Phy_To_Virt */
    fixture_mm.mmap_base = 0x40000000UL;
    assert_eq(0, mm_init_user_heap(&fixture_mm, elf_end));
    pr_set_current(&fixture_mm, PROD_ADDR_LIMIT);
}

/* Second fixture without touching `current` (predicate-only tests). */
static void setup_mm_no_current(uint64_t elf_end)
{
    pr_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = (uint64_t *)0x100000ULL;
    fixture_mm.mmap_base = 0x40000000UL;
    assert_eq(0, mm_init_user_heap(&fixture_mm, elf_end));
}

static int vma_count(void)
{
    int n = 0;
    for (list_t *p = fixture_mm.vma_list.next;
         p != &fixture_mm.vma_list; p = p->next) n++;
    return n;
}

/* Fixed MAP_FIXED mmap shorthand; returns do_mmap's raw rc. */
static int64_t fixed_mmap(uint64_t addr, uint64_t len)
{
    return do_mmap(addr, len, PROT_RW, ANON_PRIV | MAP_FIXED,
                   (uint64_t)-1, 0);
}

/* Find the VMA whose [vm_start, vm_end) contains addr, or NULL. */
static vma_t *find_vma(uint64_t addr)
{
    for (list_t *p = fixture_mm.vma_list.next;
         p != &fixture_mm.vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (addr >= v->vm_start && addr < v->vm_end) return v;
    }
    return NULL;
}

/* Auto-address mmap shorthand. */
static int64_t auto_mmap(uint64_t len)
{
    return do_mmap(0, len, PROT_RW, ANON_PRIV, (uint64_t)-1, 0);
}

/* Hinted (non-fixed) mmap shorthand. */
static int64_t hinted_mmap(uint64_t addr, uint64_t len)
{
    return do_mmap(addr, len, PROT_RW, ANON_PRIV, (uint64_t)-1, 0);
}

/* Hand-fill a PTE + dirty its backing so a "preexisting mapping"
 * is byte-observable.  Returns 0 on success, -1 if the harness
 * could not record the mapping (assertions don't abort a test, so
 * callers bail out explicitly instead of walking into a NULL). */
static int fill_pte(uint64_t va, unsigned char pattern)
{
    uint64_t phys = alloc_4k_page();
    if (phys == 0) return -1;
    if (vmm_map_4k_page(fixture_mm.pgdir, phys, va,
                        PAGE_USER | PAGE_WRITE | PAGE_VALID) != 0)
        return -1;
    pr_page_record_t *rec = pr_find_page(phys);
    if (!rec) return -1;
    ((unsigned char *)rec->backing)[0] = pattern;
    return 0;
}

/* ── Predicate: boundary / partial-overlap matrix ───────────── */

static void test_predicate_elf_envelope(void)
{
    TEST_SUITE("predicate — ELF envelope [0x400000, start_brk)");

    /* 1-page image tail: envelope is exactly [0x400000, 0x401000). */
    setup_mm_no_current(USER_CODE_ADDR + 0x1000);
    assert_eq(USER_CODE_ADDR + 0x1000, fixture_mm.start_brk);

    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x400000, 0x401000));   /* exact */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x3ff000, 0x400000 + 1)); /* 1-byte straddle head (predicate itself is unaligned-tolerant) */
    assert_false(mm_user_range_protected(&fixture_mm,
                                         0x3ff000, 0x400000));  /* touches nothing */

    /* Segment gap: image ends at 0x405000, a gap page at
     * [0x402000, 0x403000) is inside the envelope even though no
     * page is mapped there. */
    setup_mm_no_current(USER_CODE_ADDR + 0x5000);
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x402000, 0x403000));   /* gap */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x404000, 0x405000));   /* tail page */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x404000, 0x406000));   /* tail + heap */
    /* Envelope end edge: start at start_brk is the heap reserve,
     * not the envelope — still protected, via the heap clause. */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x405000, HEAP_LIMIT));
}

static void test_predicate_heap_reserve(void)
{
    TEST_SUITE("predicate — heap reserve [start_brk, HEAP_LIMIT)");

    /* Empty heap: start_brk == 0x401000, nothing committed. */
    setup_mm_no_current(USER_CODE_ADDR + 0x1000);
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x401000, 0x402000));     /* first page */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT - 0x1000, HEAP_LIMIT));   /* last page */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x401000, HEAP_LIMIT));    /* whole window */
    /* Head-edge partial: one page straddling start_brk. */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x400000, 0x402000));
    /* The zero-length heap VMA sits at [start_brk, start_brk) — a
     * request covering exactly that empty span's first byte is
     * still inside the reserve. */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        fixture_mm.start_brk,
                                        fixture_mm.start_brk + 0x1000));

    /* Grown heap: start_brk == 0x500000 — the reserve lower bound
     * moves with start_brk; the upper bound stays HEAP_LIMIT. */
    setup_mm_no_current(USER_CODE_ADDR + 0x100000);
    assert_eq(USER_CODE_ADDR + 0x100000, fixture_mm.start_brk);
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x500000, 0x501000));
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT - 0x1000, HEAP_LIMIT));
    /* Old (pre-growth) heap page is now inside the ENVELOPE —
     * still protected. */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        0x401000, 0x402000));

    /* Guard page itself is NOT the heap reserve's job. */
    setup_mm_no_current(USER_CODE_ADDR + 0x1000);
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT, USER_STACK_BASE));
}

static void test_predicate_guard_page(void)
{
    TEST_SUITE("predicate — guard page [HEAP_LIMIT, USER_STACK_BASE)");

    setup_mm_no_current(USER_CODE_ADDR + 0x1000);

    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT, USER_STACK_BASE));   /* exact */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT - 0x1000, USER_STACK_BASE));   /* head straddle */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT, USER_STACK_BASE + 0x1000));   /* tail straddle */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT - 0x1000, USER_STACK_BASE + 0x1000));   /* both sides */

    /* Above the guard the stack clause takes over; below it the
     * heap clause does — the guard clause itself has no exposed
     * "false" neighbour.  Sanity: far-away ranges are free. */
    assert_false(mm_user_range_protected(&fixture_mm,
                                         USER_STACK_END, USER_STACK_END + 0x1000));
    assert_false(mm_user_range_protected(&fixture_mm,
                                         0x200000, 0x300000));
}

static void test_predicate_user_stack(void)
{
    TEST_SUITE("predicate — user stack [USER_STACK_BASE, USER_STACK_END)");

    setup_mm_no_current(USER_CODE_ADDR + 0x1000);

    assert_true(mm_user_range_protected(&fixture_mm,
                                        USER_STACK_BASE, USER_STACK_BASE + 0x1000));   /* first page */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        USER_STACK_END - 0x1000, USER_STACK_END));   /* last page */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        USER_STACK_BASE, USER_STACK_END));   /* whole 2 MiB */
    assert_true(mm_user_range_protected(&fixture_mm,
                                        HEAP_LIMIT, USER_STACK_END + 0x1000));   /* guard+stack+above */

    assert_false(mm_user_range_protected(&fixture_mm,
                                         USER_STACK_END, USER_STACK_END + 0x1000));  /* above stack */
    assert_false(mm_user_range_protected(&fixture_mm,
                                         USER_STACK_END - 0x1000, USER_STACK_END - 0x1000));  /* empty span */
}

static void test_predicate_degenerate_inputs(void)
{
    TEST_SUITE("predicate — NULL mm / start_brk == 0 / empty span");

    /* No user image installed (init_mm, kthread): nothing protected. */
    setup_mm_no_current(USER_CODE_ADDR + 0x1000);
    fixture_mm.start_brk = 0;
    assert_false(mm_user_range_protected(&fixture_mm,
                                         USER_CODE_ADDR, USER_STACK_END));

    /* NULL mm. */
    assert_false(mm_user_range_protected(NULL, 0x400000, 0x401000));

    /* Empty span is never an intersection. */
    setup_mm_no_current(USER_CODE_ADDR + 0x1000);
    assert_false(mm_user_range_protected(&fixture_mm, 0x401000, 0x401000));
}

/* ── do_mmap(MAP_FIXED) rejection ───────────────────────────── */

static void test_mmap_fixed_rejects_protected_ranges(void)
{
    TEST_SUITE("do_mmap MAP_FIXED — -EINVAL for every protected range");

    struct { uint64_t addr, len; const char *what; } cases[] = {
        { USER_CODE_ADDR,          0x1000, "ELF envelope exact" },
        { HEAP_LIMIT - 0x1000,     0x2000, "heap tail + guard straddle" },
        { HEAP_LIMIT,              0x1000, "guard exact" },
        { USER_STACK_BASE,         0x1000, "stack first page" },
        { USER_STACK_END - 0x1000, 0x1000, "stack last page" },
        { USER_CODE_ADDR, USER_STACK_END - USER_CODE_ADDR, "whole reserve in one request" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup_mm(USER_CODE_ADDR + 0x1000);   /* start_brk = 0x401000 */
        int64_t rc = fixed_mmap(cases[i].addr, cases[i].len);
        assert_eq(-EINVAL, (int)rc);          /* cases[i].what */
        /* No partial mutation: nothing mapped, nothing unmapped,
         * no VMA inserted (only the zero-length heap VMA exists). */
        assert_eq(0, pr_state.total_maps);
        assert_eq(0, pr_state.total_unmaps);
        assert_eq(1, vma_count());
        assert_eq(0, pr_arch_tlb_flushes);
    }

    /* ELF segment gap: needs a bigger image envelope.
     * NOTE: never pass do_* calls straight to assert_eq — on failure
     * the macro re-evaluates its arguments (for the failure print),
     * so every side-effecting call must go through a variable. */
    setup_mm(USER_CODE_ADDR + 0x5000);       /* start_brk = 0x405000 */
    int64_t rc2 = fixed_mmap(0x402000, 0x1000);
    assert_eq(-EINVAL, (int)rc2);
    assert_eq(0, pr_state.total_unmaps);
    assert_eq(1, vma_count());

    /* Grown-heap page: start_brk moved up. */
    setup_mm(USER_CODE_ADDR + 0x100000);     /* start_brk = 0x500000 */
    int64_t rc3 = fixed_mmap(0x500000, 0x1000);
    assert_eq(-EINVAL, (int)rc3);
    int64_t rc4 = fixed_mmap(0x401000, 0x1000); /* now envelope */
    assert_eq(-EINVAL, (int)rc4);
    assert_eq(0, pr_state.total_unmaps);
    assert_eq(1, vma_count());
}

static void test_mmap_fixed_partial_overlap_preserves_mapping(void)
{
    TEST_SUITE("do_mmap MAP_FIXED — partial overlap leaves mapping untouched");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Preexisting managed mapping at 0x40000000 (2 pages), with
     * hand-filled PTEs and dirty backings. */
    int64_t keep = auto_mmap(0x2000);
    assert_eq((int64_t)0x40000000, keep);
    if (fill_pte(0x40000000, 0x5A) != 0 ||
        fill_pte(0x40001000, 0xA5) != 0) {
        assert_true(!"harness fill_pte failed");
        return;
    }

    pr_pte_record_t *p0 = pr_find_mapping(0x40000000);
    pr_pte_record_t *p1 = pr_find_mapping(0x40001000);
    assert_not_null(p0);
    assert_not_null(p1);
    uint64_t pte0 = p0->pte, pte1 = p1->pte;

    int maps_before   = pr_state.total_maps;
    int unmaps_before = pr_state.total_unmaps;
    int vmas_before   = vma_count();

    /* Rejected request PARTIALLY overlapping the guard page — the
     * other end sits in the free heap reserve.  Must fail with no
     * call to do_munmap_locked (nothing else would be affected
     * anyway, but the counters prove zero side effects). */
    int64_t rc = fixed_mmap(HEAP_LIMIT - 0x1000, 0x2000);
    assert_eq(-EINVAL, (int)rc);

    /* Existing mapping byte-for-byte intact. */
    assert_eq(pte0, pr_find_mapping(0x40000000)->pte);
    assert_eq(pte1, pr_find_mapping(0x40001000)->pte);
    assert_eq(0x5A, (int)((unsigned char *)pr_find_page(pte0 & PAGE_4K_MASK)->backing)[0]);
    assert_eq(0xA5, (int)((unsigned char *)pr_find_page(pte1 & PAGE_4K_MASK)->backing)[0]);
    assert_eq(maps_before,   pr_state.total_maps);
    assert_eq(unmaps_before, pr_state.total_unmaps);
    assert_eq(vmas_before,   vma_count());
    assert_eq(0, pr_arch_tlb_flushes);

    /* Second probe: request overlapping the STACK's first page —
     * same untouched contract. */
    rc = fixed_mmap(HEAP_LIMIT, 0x2000);
    assert_eq(-EINVAL, (int)rc);
    assert_eq(pte0, pr_find_mapping(0x40000000)->pte);
    assert_eq(pte1, pr_find_mapping(0x40001000)->pte);
    assert_eq(unmaps_before, pr_state.total_unmaps);
    assert_eq(vmas_before,   vma_count());
}

/* ── Focus #4 exact scenario: rejected MAP_FIXED that PARTLY
 * OVERLAPS a managed range must leave that mapping untouched ── */

static void test_mmap_fixed_partial_overlap_managed_stack_top(void)
{
    TEST_SUITE("MAP_FIXED — request overlapping a managed VMA at the stack-top boundary");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* A LEGITIMATE managed mapping just above the window end:
     * [0x1600000, 0x1602000). */
    int64_t keep = fixed_mmap(USER_STACK_END, 0x2000);
    assert_eq((int64_t)USER_STACK_END, keep);
    if (fill_pte(USER_STACK_END, 0x21) != 0 ||
        fill_pte(USER_STACK_END + 0x1000, 0x22) != 0) {
        assert_true(!"harness fill_pte failed");
        return;
    }
    vma_t *kv = find_vma(USER_STACK_END);
    assert_not_null(kv);
    assert_eq((int64_t)USER_STACK_END, (int64_t)kv->vm_start);
    assert_eq((int64_t)(USER_STACK_END + 0x2000), (int64_t)kv->vm_end);

    int vmas_before   = vma_count();
    int unmaps_before = pr_state.total_unmaps;
    int flushes_before = (int)pr_arch_tlb_flushes;
    uint64_t pte_keep = pr_find_mapping(USER_STACK_END)->pte;
    assert_true(pte_keep != 0);

    /* Rejected request [0x15ff000, 0x1601000): its first page hits
     * the stack clause, its second page is the FIRST page of the
     * managed mapping.  A do_munmap_locked call here would truncate
     * keep's left side to [0x1601000, 0x1602000). */
    int64_t rc = fixed_mmap(USER_STACK_END - 0x1000, 0x2000);
    assert_eq(-EINVAL, (int)rc);

    /* keep untouched: original bounds (NOT truncated), same PTE,
     * same dirty byte, no VMA inserted or split, no unmap, no
     * flush. */
    assert_eq((int64_t)USER_STACK_END, (int64_t)kv->vm_start);
    assert_eq((int64_t)(USER_STACK_END + 0x2000), (int64_t)kv->vm_end);
    assert_eq((int64_t)pte_keep, (int64_t)pr_find_mapping(USER_STACK_END)->pte);
    assert_eq(0x21, (int)((unsigned char *)
                  pr_find_page(pte_keep & PAGE_4K_MASK)->backing)[0]);
    assert_eq(vmas_before,   vma_count());
    assert_eq(unmaps_before, pr_state.total_unmaps);
    assert_eq(flushes_before, (int)pr_arch_tlb_flushes);
}

static void test_mmap_fixed_partial_overlap_managed_guard_straddle(void)
{
    TEST_SUITE("MAP_FIXED — request strictly inside a managed VMA straddling the guard page");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Simulate the managed range a pre-Task-5 world could hold: a
     * VMA straddling heap→guard→stack at [HEAP_LIMIT - 0x1000, USER_STACK_BASE + 0x1000),
     * inserted through the production vma_insert + hand-filled
     * PTEs.  Post-Task-5 the mapping APIs can no longer create
     * this shape — the test proves they cannot DESTROY it either. */
    vma_t *sv = (vma_t *)kmalloc(sizeof(vma_t));
    assert_not_null(sv);
    list_init(&sv->list);
    sv->vm_start     = HEAP_LIMIT - 0x1000;
    sv->vm_end       = USER_STACK_BASE + 0x1000;
    sv->vm_flags     = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON;
    sv->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    sv->vm_pgoff     = 0;
    sv->vm_file      = NULL;
    assert_eq(0, vma_insert(&fixture_mm, sv));
    if (fill_pte(HEAP_LIMIT - 0x1000, 0x31) != 0 ||
        fill_pte(HEAP_LIMIT, 0x32) != 0 ||
        fill_pte(USER_STACK_BASE, 0x33) != 0) {
        assert_true(!"harness fill_pte failed");
        return;
    }

    int vmas_before   = vma_count();   /* heap VMA + sv = 2 */
    int unmaps_before = pr_state.total_unmaps;
    int flushes_before = (int)pr_arch_tlb_flushes;
    uint64_t pte_head = pr_find_mapping(HEAP_LIMIT - 0x1000)->pte;
    uint64_t pte_mid  = pr_find_mapping(HEAP_LIMIT)->pte;
    uint64_t pte_tail = pr_find_mapping(USER_STACK_BASE)->pte;

    /* Guard-exact request [HEAP_LIMIT, USER_STACK_BASE) lies STRICTLY
     * INSIDE sv — a do_munmap_locked call would split it into
     * [HEAP_LIMIT - 0x1000, HEAP_LIMIT) + [USER_STACK_BASE, USER_STACK_BASE + 0x1000) and unmap
     * the guard page. */
    int64_t rc = fixed_mmap(HEAP_LIMIT, 0x1000);
    assert_eq(-EINVAL, (int)rc);

    /* No split, no truncation: sv is still ONE VMA with the
     * original bounds, all three PTEs and their dirty bytes
     * intact, counters unchanged. */
    assert_eq(HEAP_LIMIT - 0x1000, sv->vm_start);
    assert_eq(USER_STACK_BASE + 0x1000, sv->vm_end);
    assert_eq(vmas_before,   vma_count());
    assert_eq((int64_t)pte_head, (int64_t)pr_find_mapping(HEAP_LIMIT - 0x1000)->pte);
    assert_eq((int64_t)pte_mid,  (int64_t)pr_find_mapping(HEAP_LIMIT)->pte);
    assert_eq((int64_t)pte_tail, (int64_t)pr_find_mapping(USER_STACK_BASE)->pte);
    assert_eq(0x31, (int)((unsigned char *)
                  pr_find_page(pte_head & PAGE_4K_MASK)->backing)[0]);
    assert_eq(0x32, (int)((unsigned char *)
                  pr_find_page(pte_mid & PAGE_4K_MASK)->backing)[0]);
    assert_eq(0x33, (int)((unsigned char *)
                  pr_find_page(pte_tail & PAGE_4K_MASK)->backing)[0]);
    assert_eq(unmaps_before,  pr_state.total_unmaps);
    assert_eq(flushes_before, (int)pr_arch_tlb_flushes);
}

static void test_mmap_auto_and_hint_avoid_reserve(void)
{
    TEST_SUITE("do_mmap — auto + hinted (non-fixed) never land in the reserve");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Plain auto mmap: must come back outside the entire
     * protected window. */
    int64_t a = auto_mmap(0x1000);
    assert_true(a >= (int64_t)USER_STACK_END);
    assert_eq((int64_t)0x40000000, a);   /* mmap_base, as before Task 5 */

    /* Hint pointing into the (empty) heap reserve: WITHOUT
     * MAP_FIXED the hint is advisory — the kernel must not place
     * the mapping there (and must not fail either). */
    int64_t b = hinted_mmap(0x500000, 0x1000);
    assert_true(b >= (int64_t)USER_STACK_END);
    assert_true((uint64_t)b < PROD_ADDR_LIMIT);

    /* Hint on the guard page and on the stack base: same. */
    int64_t c = hinted_mmap(HEAP_LIMIT, 0x1000);
    assert_true(c >= (int64_t)USER_STACK_END);
    int64_t d = hinted_mmap(USER_STACK_BASE, 0x1000);
    assert_true(d >= (int64_t)USER_STACK_END);

    /* All three landed outside the reserve and are distinct. */
    assert_true((uint64_t)b != (uint64_t)c);
    assert_true((uint64_t)c != (uint64_t)d);

    /* Control: a legitimate MAP_FIXED well above the reserve
     * still succeeds. */
    int64_t e = fixed_mmap(0x40000000 + 0x100000, 0x1000);
    assert_eq((int64_t)0x40100000, e);
    /* heap + a + b + c + d + e */
    assert_eq(6, vma_count());
}

static void test_mmap_overflow_and_above_limit_no_mutation(void)
{
    TEST_SUITE("do_mmap — overflow / above-user-limit fail without mutation");

    setup_mm(USER_CODE_ADDR + 0x1000);

    int maps_before   = pr_state.total_maps;
    int unmaps_before = pr_state.total_unmaps;
    int vmas_before   = vma_count();

    /* Address-space overflow (addr + length wraps past 2^64). */
    int64_t rc = do_mmap(0x40000000, 0xfffffffff0000000UL, PROT_RW,
                         ANON_PRIV | MAP_FIXED, (uint64_t)-1, 0);
    assert_eq(-EINVAL, (int)rc);

    /* At / above the user limit. */
    rc = do_mmap(PROD_ADDR_LIMIT, 0x1000, PROT_RW,
                 ANON_PRIV | MAP_FIXED, (uint64_t)-1, 0);
    assert_eq(-ENOMEM, (int)rc);
    rc = do_mmap(PROD_ADDR_LIMIT + 0x1000, 0x1000, PROT_RW,
                 ANON_PRIV | MAP_FIXED, (uint64_t)-1, 0);
    assert_eq(-ENOMEM, (int)rc);

    /* Guard-page request that runs to the top of the address
     * space: intersects the reserve, so Task 5 rejects it with
     * -EINVAL (pre-Task-5 it fell through to the addr_limit check
     * and came back -ENOMEM, having already torn the heap VMA out
     * via do_munmap_locked — the vma_count assert below catches
     * exactly that side effect). */
    rc = do_mmap(HEAP_LIMIT, 0xfffffffff0000000UL, PROT_RW,
                 ANON_PRIV | MAP_FIXED, (uint64_t)-1, 0);
    assert_eq(-EINVAL, (int)rc);

    assert_eq(maps_before,   pr_state.total_maps);
    assert_eq(unmaps_before, pr_state.total_unmaps);
    assert_eq(vmas_before,   vma_count());
}

/* ── do_munmap / do_mprotect rejection ──────────────────────── */

static void test_munmap_rejects_protected_ranges(void)
{
    TEST_SUITE("do_munmap — protected range (incl. straddle) → -EINVAL");

    setup_mm(USER_CODE_ADDR + 0x1000);

    /* Control: the harness really observes unmaps — map 1 page and
     * release it. */
    int64_t keep = auto_mmap(0x1000);
    assert_eq((int64_t)0x40000000, keep);
    if (fill_pte(0x40000000, 0x11) != 0) {
        assert_true(!"harness fill_pte failed");
        return;
    }
    int64_t ctl = do_munmap(0x40000000, 0x1000);
    assert_eq(0, (int)ctl);
    assert_eq(1, pr_state.total_unmaps);
    assert_null(pr_find_mapping(0x40000000));

    int unmaps_before = pr_state.total_unmaps;
    int flushes_before = (int)pr_arch_tlb_flushes;

    /* Exact guard page. */
    int64_t r1 = do_munmap(HEAP_LIMIT, 0x1000);
    assert_eq(-EINVAL, (int)r1);
    /* Straddle: heap tail + guard + stack head. */
    r1 = do_munmap(HEAP_LIMIT - 0x1000, 0x3000);
    assert_eq(-EINVAL, (int)r1);
    /* Stack page. */
    r1 = do_munmap(USER_STACK_BASE, 0x1000);
    assert_eq(-EINVAL, (int)r1);
    /* ELF envelope page. */
    r1 = do_munmap(0x400000, 0x1000);
    assert_eq(-EINVAL, (int)r1);
    /* Heap reserve page (uncommitted — the reserve is protected,
     * not just committed leaves). */
    r1 = do_munmap(0x401000, 0x1000);
    assert_eq(-EINVAL, (int)r1);

    assert_eq(unmaps_before, pr_state.total_unmaps);
    assert_eq(flushes_before, (int)pr_arch_tlb_flushes);
    assert_eq(1, vma_count());   /* only the zero-length heap VMA */
}

static void test_mprotect_rejects_protected_ranges(void)
{
    TEST_SUITE("do_mprotect — protected range (incl. straddle) → -EINVAL");

    setup_mm(USER_CODE_ADDR + 0x1000);

    int flushes_before = (int)pr_arch_tlb_flushes;

    /* Exact guard page, PROT_NONE and PROT_READ variants. */
    int64_t r = do_mprotect(HEAP_LIMIT, 0x1000, PROT_NONE);
    assert_eq(-EINVAL, (int)r);
    r = do_mprotect(HEAP_LIMIT, 0x1000, PROT_READ);
    assert_eq(-EINVAL, (int)r);
    /* Heap reserve page. */
    r = do_mprotect(0x401000, 0x1000, PROT_READ);
    assert_eq(-EINVAL, (int)r);
    /* ELF envelope page. */
    r = do_mprotect(0x400000, 0x1000, PROT_READ);
    assert_eq(-EINVAL, (int)r);
    /* Stack page. */
    r = do_mprotect(USER_STACK_BASE, 0x1000, PROT_READ);
    assert_eq(-EINVAL, (int)r);
    /* Straddle heap→guard→stack. */
    r = do_mprotect(HEAP_LIMIT - 0x1000, 0x3000, PROT_READ);
    assert_eq(-EINVAL, (int)r);

    assert_eq(flushes_before, (int)pr_arch_tlb_flushes);
    assert_eq(1, vma_count());

    /* Control: mprotect on a real (non-protected) mapping still
     * flips the VMA flags. */
    int64_t keep = auto_mmap(0x1000);
    assert_eq((int64_t)0x40000000, keep);
    int64_t ctl = do_mprotect(0x40000000, 0x1000, PROT_READ);
    assert_eq(0, (int)ctl);
    for (list_t *p = fixture_mm.vma_list.next;
         p != &fixture_mm.vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_start == 0x40000000) {
            assert_eq((uint64_t)PROT_READ & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC),
                      v->vm_flags & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC));
        }
    }
}

/* ── Source-level inspection: wiring order inside vma.c ─────── */

static const char *vma_c_path(void)
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
                 "%.*s/kernel/memory/vma.c",
                 (int)(marker - full), full);
        return buf;
    }
    if (strncmp(full, "hosttests/", 10) == 0)
        return "kernel/memory/vma.c";
    return NULL;
}

static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return NULL;
    size_t cap = 16384, len = 0;
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

static const char *find_from(const char *start, const char *end,
                             const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = start; p + n <= end; p++) {
        if (memcmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

static void test_wiring_order_in_vma_c(void)
{
    TEST_SUITE("vma.c source — predicate precedes every mutation/callback");

    size_t len;
    char *src = slurp(vma_c_path(), &len);
    assert_not_null(src);
    const char *end = src + len;

    /* do_munmap_locked: predicate before the first PTE unmap. */
    const char *fn = strstr(src, "static int64_t do_munmap_locked(");
    assert_not_null(fn);
    const char *first_unmap = find_from(fn, end, "vmm_unmap_4k_page(");
    assert_not_null(first_unmap);
    const char *chk = find_from(fn, first_unmap, "mm_user_range_protected(");
    assert_not_null(chk);

    /* do_mmap: predicate before the MAP_FIXED do_munmap_locked call
     * and before the device callback invocation. */
    fn = strstr(src, "int64_t do_mmap(uint64_t addr");
    assert_not_null(fn);
    const char *fixed_unmap = find_from(fn, end, "do_munmap_locked(addr, length)");
    assert_not_null(fixed_unmap);
    chk = find_from(fn, fixed_unmap, "mm_user_range_protected(");
    assert_not_null(chk);

    const char *dev_call = find_from(fn, end, "_dev_mmap(file_node");
    assert_not_null(dev_call);
    chk = find_from(fn, dev_call, "mm_user_range_protected(");
    assert_not_null(chk);

    /* Device branch, stronger: the predicate check must sit at the
     * TOP of the `if (_dev_mmap)` block — before the VMA
     * pre-allocation the handler fills PTEs through (i.e. before
     * ANY work in that branch, not just before the call itself). */
    const char *dev_branch = find_from(fn, end, "if (_dev_mmap) {");
    assert_not_null(dev_branch);
    const char *dev_alloc = find_from(dev_branch, end, "kmalloc(sizeof(vma_t))");
    assert_not_null(dev_alloc);
    chk = find_from(dev_branch, dev_alloc, "mm_user_range_protected(");
    assert_not_null(chk);
    /* ...and before the callback invocation too. */
    chk = find_from(dev_branch, dev_call, "mm_user_range_protected(");
    assert_not_null(chk);

    /* do_mprotect: predicate before the first VMA-flag write. */
    fn = strstr(src, "int64_t do_mprotect(uint64_t addr");
    assert_not_null(fn);
    const char *flag_write = find_from(fn, end, "v->vm_flags");
    assert_not_null(flag_write);
    chk = find_from(fn, flag_write, "mm_user_range_protected(");
    assert_not_null(chk);

    /* Auto-search skip: the non-fixed branch must reference the
     * predicate (defensive skip past the whole window). */
    fn = strstr(src, "int64_t do_mmap(uint64_t addr");
    assert_not_null(fn);
    const char *nonfixed = strstr(fn, "if (!(flags & MAP_FIXED)) {");
    assert_not_null(nonfixed);
    const char *fixed_branch = find_from(nonfixed, end, "} else {");
    assert_not_null(fixed_branch);
    chk = find_from(nonfixed, fixed_branch, "mm_user_range_protected(");
    assert_not_null(chk);

    free(src);
}

/* ── Test list ──────────────────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_predicate_elf_envelope),
    TEST_ENTRY(test_predicate_heap_reserve),
    TEST_ENTRY(test_predicate_guard_page),
    TEST_ENTRY(test_predicate_user_stack),
    TEST_ENTRY(test_predicate_degenerate_inputs),
    TEST_ENTRY(test_mmap_fixed_rejects_protected_ranges),
    TEST_ENTRY(test_mmap_fixed_partial_overlap_preserves_mapping),
    TEST_ENTRY(test_mmap_fixed_partial_overlap_managed_stack_top),
    TEST_ENTRY(test_mmap_fixed_partial_overlap_managed_guard_straddle),
    TEST_ENTRY(test_mmap_auto_and_hint_avoid_reserve),
    TEST_ENTRY(test_mmap_overflow_and_above_limit_no_mutation),
    TEST_ENTRY(test_munmap_rejects_protected_ranges),
    TEST_ENTRY(test_mprotect_rejects_protected_ranges),
    TEST_ENTRY(test_wiring_order_in_vma_c),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed == 0 ? 0 : 1;
}
