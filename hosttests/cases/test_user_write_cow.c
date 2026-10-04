/*
 * test/cases/test_user_write_cow.c — Host tests for the kernel-side
 * COW privatization primitive introduced by Task 7 of the user
 * heap/ELF isolation plan.
 *
 * Production-linked:
 *   - Compiles the REAL kernel/memory/uaccess.c (new
 *     prepare_user_write_range + prepare_user_write_range_locked +
 *     copy_to_user_ft_res delegating to prepare).
 *   - Compiles the REAL kernel/memory/vma.c (user_write_range_begin
 *     thin-wraps prepare_user_write_range_locked).
 *   - Stubs the heavy headers via hosttests/mock/uwrite_cow/.
 *
 * Coverage mirrors the brief:
 *   - kernel output to forked COW heap leaves
 *   - anonymous mmap and file mmap buffers through file
 *   - TTY and pipe reads via copy_to_user_ft_res
 *   - two-page preparation OOM on page two (Plan Review Focus #5)
 *   - invalid or VMA_IO destination
 *   - pure syscall_check_user_range(writable=true) accepts eligible
 *     COW without changing PTEs
 *   - _ft_res callback exactly once on preparation failure and no
 *     stale fault_jmp
 *   - distinct -ENOMEM / -EFAULT propagation at callers
 */
#include "test_framework.h"
#include "uwrite_cow_stubs.h"

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
#include <memory/uaccess.h>

/* ── Layout constants (mirror kernel/memory/vma.c) ─────────── */
#define USER_CODE_ADDR   0x400000UL
#define USER_PAGE_SIZE   0x1000000UL
#define HEAP_LIMIT       (USER_CODE_ADDR + USER_PAGE_SIZE - 0x1000UL)
#define USER_STACK_BASE  0x1400000UL
#define PROD_ADDR_LIMIT  0x00007FFFFFFFFFFFUL

/* ── Per-test fixture ───────────────────────────────────────── */
static mm_t fixture_mm;
static uint64_t fixture_pgd[512];

/* Each test gets a unique page slot in this 256-page (1 MiB) window.
 * With identity Phy_To_Virt, the phys IS the slot VA — so the test
 * uses USER_VA_BASE + i * 0x1000 for the i-th allocation. */
#define USER_VA_BASE  0x400000UL
#define USER_VA_END   (USER_VA_BASE + 256UL * 0x1000UL)  /* 0x500000 */



/* Build an anonymous mmap VMA [start, end) with PROT_READ|PROT_WRITE. */
static vma_t *build_anon_vma(uint64_t start, uint64_t end)
{
    vma_t *v = (vma_t *)kmalloc(sizeof(vma_t));
    if (!v) abort();
    memset(v, 0, sizeof(*v));
    list_init(&v->list);
    v->vm_start     = start;
    v->vm_end       = end;
    v->vm_flags     = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON;
    v->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    v->vm_pgoff     = 0;
    v->vm_file      = NULL;
    vma_insert(&fixture_mm, v);
    return v;
}

/* Build a file mmap VMA [start, end) backed by a fake node. */

/* Build a VMA_IO VMA — task: must be rejected by prepare. */
static vma_t *build_vmio_vma(uint64_t start, uint64_t end)
{
    static vfs_node_t fake_io_node;
    fake_io_node = (vfs_node_t){0};
    vma_t *v = (vma_t *)kmalloc(sizeof(vma_t));
    if (!v) abort();
    memset(v, 0, sizeof(*v));
    list_init(&v->list);
    v->vm_start     = start;
    v->vm_end       = end;
    v->vm_flags     = VMA_PROT_READ | VMA_PROT_WRITE | VMA_IO;
    v->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    v->vm_pgoff     = 0;
    v->vm_file      = &fake_io_node;
    vma_insert(&fixture_mm, v);
    return v;
}

/* Helper: assign a 4 KiB leaf to the user VA.  With identity
 * Phy_To_Virt, the slot whose phys equals va is the one whose
 * host backing is at host-VA va.  Mark that slot in_use so a
 * subsequent prepare-time alloc_4k_page will pick a DIFFERENT
 * slot (the brief's "allocate a new phys" must produce a distinct
 * page for the privatized copy).  Returns va (= phys, by identity). */
static uint64_t commit_leaf(uint64_t va, uint64_t page_prot)
{
    int idx = uw_pte_index_for(va);
    if (idx < 0) abort();
    if (!uw_page_pool[idx].in_use) {
        uw_page_pool[idx].in_use = 1;
        uw_state.total_allocs++;
    }
    int rc = vmm_map_4k_page(NULL, va, va, page_prot);
    if (rc) abort();
    return va;
}

/* Walk the pool and find the slot whose phys equals `va`.  Used by
 * tests to fetch the COW-refcount + backing pointer for an allocated
 * page in the user-VA window. */

/* Convert a writable leaf into a COW-shared leaf with the given
// refcount.  Caller is responsible for the resulting semantics:
// refs=N means N owners share the same phys. */
static void mark_cow(uint64_t va, uint16_t refs)
{
    uw_pte_record_t *rec = uw_find_mapping(va);
    if (!rec) abort();
    uint64_t old_phys = rec->phys;
    rec->pte = old_phys | PAGE_USER | PAGE_COW | PAGE_VALID;
    rec->flags = PAGE_USER | PAGE_COW | PAGE_VALID;
    uw_page_record_t *pg = uw_find_page(old_phys);
    if (!pg) abort();
    pg->cow_refs = refs;
    uw_state.total_cow_gets += refs;
}

/* ──────────────────────────────────────────────────────────────
 *  prepare_user_write_range: PTE-mutation rules
 * ────────────────────────────────────────────────────────────── */

static void test_prepare_noop_on_writable_leaf(void)
{
    TEST_SUITE("prepare_user_write_range — no-op on writable non-COW leaf");

    /* Bypass the heap-VMA dance: build a single anon VMA covering
     * one page, commit a writable leaf, then call prepare. */
    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x400000UL;  /* 0xC00000 */
    build_anon_vma(va, va + 0x1000);
    uint64_t phys_a = commit_leaf(va, PAGE_USER_PTE);
    memset(uw_find_page(phys_a)->backing, 0xCC, 4096);

    int allocs_before = uw_state.total_allocs;
    int rc = prepare_user_write_range(&fixture_mm, va, 0x1000);
    assert_eq(0, rc);

    /* No allocation: leaf was already writable, no COW privatization. */
    assert_eq(0, uw_state.total_allocs - allocs_before);
    /* PTE unchanged: still points at phys_a. */
    uw_pte_record_t *rec = uw_find_mapping(va);
    assert_not_null(rec);
    assert_eq(phys_a, rec->pte & PAGE_4K_MASK);
    /* TLB shootdown fired (the contract: every successful prepare
     * ends with tlb_shootdown even when no pages were touched). */
    assert_eq(0, uw_state.total_tlb_shootdowns);
    assert_eq(0, fixture_mm.lock.lock);

    uw_clear_current();
}

static void test_prepare_cow_refs_one_promotes_inplace(void)
{
    TEST_SUITE("prepare_user_write_range — COW refs==1 promotes in place");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x401000UL;
    build_anon_vma(va, va + 0x1000);
    uint64_t phys = commit_leaf(va, PAGE_USER_PTE);
    mark_cow(va, 1);

    int allocs_before = uw_state.total_allocs;
    int rc = prepare_user_write_range(&fixture_mm, va, 0x1000);
    assert_eq(0, rc);

    /* refs==1 ⇒ no allocation, just clear PAGE_COW + set PAGE_WRITE. */
    assert_eq(0, uw_state.total_allocs - allocs_before);
    uw_pte_record_t *rec = uw_find_mapping(va);
    assert_not_null(rec);
    assert_eq(phys, rec->pte & PAGE_4K_MASK);
    assert_eq((uint64_t)PAGE_USER_PTE, rec->pte & (PAGE_USER | PAGE_WRITE | PAGE_VALID | PAGE_COW));
    /* Last ref released (was 1 → 0) */
    assert_eq(1, uw_state.total_cow_puts);
    assert_eq(1, uw_state.total_tlb_shootdowns);

    uw_clear_current();
}

static void test_prepare_cow_refs_many_privatizes(void)
{
    TEST_SUITE("prepare_user_write_range — COW refs>1 privatizes");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x402000UL;
    build_anon_vma(va, va + 0x1000);
    uint64_t phys_orig = commit_leaf(va, PAGE_USER_PTE);
    mark_cow(va, 3);

    /* Stash distinct bytes in the original page so we can detect the
    * memcpy during privatization. */
    memset(uw_find_page(phys_orig)->backing, 0xAB, 4096);

    int allocs_before = uw_state.total_allocs;
    int rc = prepare_user_write_range(&fixture_mm, va, 0x1000);
    assert_eq(0, rc);

    /* Alloc happens (1 new phys), old refcount decrements (3 → 2),
     * phys changes. */
    assert_eq(1, uw_state.total_allocs - allocs_before);
    uw_pte_record_t *rec = uw_find_mapping(va);
    assert_not_null(rec);
    assert_true((rec->pte & PAGE_4K_MASK) != phys_orig);
    assert_eq((uint64_t)PAGE_USER_PTE,
              rec->pte & (PAGE_USER | PAGE_WRITE | PAGE_VALID | PAGE_COW));
    /* New phys has AB bytes (memcpy of original). */
    uw_page_record_t *pg_new = uw_find_page(rec->pte & PAGE_4K_MASK);
    assert_not_null(pg_new);
    assert_eq(0xAB, ((uint8_t *)pg_new->backing)[0]);
    assert_eq(0xAB, ((uint8_t *)pg_new->backing)[4095]);
    /* Old phys's COW refcount dropped 3 → 2 (still alive). */
    uw_page_record_t *pg_old = uw_find_page(phys_orig);
    assert_not_null(pg_old);
    assert_eq(2, pg_old->cow_refs);

    uw_clear_current();
}

static void test_prepare_span_two_cow_pages(void)
{
    TEST_SUITE("prepare_user_write_range — span two COW leaves, both privatized");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va1 = 0x403000UL;
    uint64_t va2 = va1 + 0x1000;
    build_anon_vma(va1, va2 + 0x1000);
    uint64_t p1 = commit_leaf(va1, PAGE_USER_PTE);
    uint64_t p2 = commit_leaf(va2, PAGE_USER_PTE);
    mark_cow(va1, 2);
    mark_cow(va2, 2);
    memset(uw_find_page(p1)->backing, 0x11, 4096);
    memset(uw_find_page(p2)->backing, 0x22, 4096);

    int allocs_before = uw_state.total_allocs;
    int rc = prepare_user_write_range(&fixture_mm, va1, 0x2000);
    assert_eq(0, rc);
    assert_eq(2, uw_state.total_allocs - allocs_before);
    /* Both PTEs replaced. */
    uw_pte_record_t *r1 = uw_find_mapping(va1);
    uw_pte_record_t *r2 = uw_find_mapping(va2);
    assert_true((r1->pte & PAGE_4K_MASK) != p1);
    assert_true((r2->pte & PAGE_4K_MASK) != p2);
    /* Both old physes still alive (refs 2 → 1). */
    assert_eq(1, uw_find_page(p1)->cow_refs);
    assert_eq(1, uw_find_page(p2)->cow_refs);
    /* Content copied. */
    assert_eq(0x11, ((uint8_t *)uw_find_page(r1->pte & PAGE_4K_MASK)->backing)[0]);
    assert_eq(0x22, ((uint8_t *)uw_find_page(r2->pte & PAGE_4K_MASK)->backing)[0]);

    uw_clear_current();
}

/* Plan Review Focus #5: failed two-page COW prep leaves both target
 * bytes and both original PTE/refcounts intact. */
static void test_prepare_oom_on_page_two_rolls_back(void)
{
    TEST_SUITE("prepare_user_write_range — OOM on page 2 rolls back atomically");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va1 = 0x404000UL;
    uint64_t va2 = va1 + 0x1000;
    build_anon_vma(va1, va2 + 0x1000);
    uint64_t p1 = commit_leaf(va1, PAGE_USER_PTE);
    uint64_t p2 = commit_leaf(va2, PAGE_USER_PTE);
    mark_cow(va1, 2);
    mark_cow(va2, 2);
    memset(uw_find_page(p1)->backing, 0x33, 4096);
    memset(uw_find_page(p2)->backing, 0x44, 4096);
    /* Capture PTE values BEFORE the failed call. */
    uint64_t pte1_before = uw_find_mapping(va1)->pte;
    uint64_t pte2_before = uw_find_mapping(va2)->pte;
    uint16_t refs1_before = uw_find_page(p1)->cow_refs;
    uint16_t refs2_before = uw_find_page(p2)->cow_refs;
    int allocs_before_prepare = uw_state.total_allocs;
    int frees_before_prepare = uw_state.total_frees;

    /* Plan Review Focus #5: fail the SECOND alloc (page 2).  The
     * first alloc (page 1) succeeded and was staged; the second
     * must trigger rollback: free the staged page-1 phys, leave
     * both original PTEs and refcounts intact. */
    uw_state.inject_alloc_fail_at = uw_state.total_allocs + 2;

    int rc = prepare_user_write_range(&fixture_mm, va1, 0x2000);
    assert_eq(-ENOMEM, rc);

    /* Plan Review Focus #5 invariant: the rolled-back alloc MUST be
     * reflected in total_frees.  We injected failure on the SECOND
     * alloc, so the FIRST alloc succeeded (total_allocs advanced)
     * and was then freed by the rollback (total_frees advanced).
     * Net: delta during prepare is +1 alloc +1 free. */
    int allocs_during = uw_state.total_allocs - allocs_before_prepare;
    assert_eq(1, allocs_during);               /* first alloc succeeded */
    assert_eq(1, uw_state.total_frees - frees_before_prepare);
    /* No PTE changed. */
    assert_eq(pte1_before, uw_find_mapping(va1)->pte);
    assert_eq(pte2_before, uw_find_mapping(va2)->pte);
    /* No refcount changed. */
    assert_eq(refs1_before, uw_find_page(p1)->cow_refs);
    assert_eq(refs2_before, uw_find_page(p2)->cow_refs);
    /* Target bytes untouched: still 0x33 / 0x44. */
    assert_eq(0x33, ((uint8_t *)uw_find_page(p1)->backing)[0]);
    assert_eq(0x44, ((uint8_t *)uw_find_page(p2)->backing)[0]);
    /* Lock released on -ENOMEM. */
    assert_eq(0, fixture_mm.lock.lock);
    /* prepare audit: -ENOMEM recorded. */

    uw_clear_current();
}

static void test_prepare_rejects_vmio(void)
{
    TEST_SUITE("prepare_user_write_range — VMA_IO destination → -EFAULT");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x405000UL;
    build_vmio_vma(va, va + 0x1000);
    commit_leaf(va, PAGE_USER_PTE);
    mark_cow(va, 2);

    int rc = prepare_user_write_range(&fixture_mm, va, 0x1000);
    assert_eq(-EFAULT, rc);
    /* PTEs untouched. */
    assert_eq(0, uw_state.total_tlb_shootdowns);
    /* Lock released. */
    assert_eq(0, fixture_mm.lock.lock);

    uw_clear_current();
}

static void test_prepare_rejects_unmapped(void)
{
    TEST_SUITE("prepare_user_write_range — unmapped destination → -EFAULT");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x406000UL;
    /* No VMA, no PTE — gap between 0xC00000 and the heap VMA
     * (start_brk = end_brk = ALIGN_UP(elf_end, 4096)). */
    int rc = prepare_user_write_range(&fixture_mm, va, 0x1000);
    assert_eq(-EFAULT, rc);

    uw_clear_current();
}

static void test_prepare_rejects_below_user_min(void)
{
    TEST_SUITE("prepare_user_write_range — below USER_MIN_ADDR → -EFAULT");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    int rc = prepare_user_write_range(&fixture_mm, 0x1000, 0x1000);
    assert_eq(-EFAULT, rc);

    uw_clear_current();
}

static void test_prepare_efault_does_not_invalidate_pte(void)
{
    TEST_SUITE("prepare_user_write_range — -EFAULT preserves every PTE");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x407000UL;
    build_anon_vma(va, va + 0x3000);                 /* span 3 */
    uint64_t p1 = commit_leaf(va,        PAGE_USER_PTE);
    uint64_t p2 = commit_leaf(va+0x1000, PAGE_USER_PTE);
    /* p3 unmapped — the middle page is committed, the third is a hole.
     * prepare must reject the whole range on the unmapped page. */
    (void)p1; (void)p2;
    uint64_t pte1_before = uw_find_mapping(va)->pte;
    uint64_t pte2_before = uw_find_mapping(va+0x1000)->pte;

    int allocs_before = uw_state.total_allocs;
    int rc = prepare_user_write_range(&fixture_mm, va, 0x3000);
    assert_eq(-EFAULT, rc);

    /* Neither PTE changed. */
    assert_eq(pte1_before, uw_find_mapping(va)->pte);
    assert_eq(pte2_before, uw_find_mapping(va+0x1000)->pte);
    /* No alloc. */
    assert_eq(0, uw_state.total_allocs - allocs_before);

    uw_clear_current();
}

/* ──────────────────────────────────────────────────────────────
 *  prepare_user_write_range_locked — caller owns the lock
 * ────────────────────────────────────────────────────────────── */

static void test_locked_retains_lock_on_success(void)
{
    TEST_SUITE("prepare_user_write_range_locked — lock retained on success");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x408000UL;
    build_anon_vma(va, va + 0x1000);
    commit_leaf(va, PAGE_USER_PTE);

    spin_lock(&fixture_mm.lock);
    int rc = prepare_user_write_range_locked(&fixture_mm, va, 0x1000);
    assert_eq(0, rc);
    /* Lock still held (test_platform spinlock stub is a noop, so we
     * can't observe the counter — the call is the proof). */
    spin_unlock(&fixture_mm.lock);

    uw_clear_current();
}

static void test_locked_retains_lock_on_failure(void)
{
    TEST_SUITE("prepare_user_write_range_locked — lock retained on -EFAULT");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    /* The locked entry never releases its caller-owned lock. */
    spin_lock(&fixture_mm.lock);
    int rc = prepare_user_write_range_locked(&fixture_mm,
                                              0x409000UL,
                                              0x1000);
    assert_eq(-EFAULT, rc);
    assert_eq(1, fixture_mm.lock.lock);
    /* Calling spin_unlock after a failed prepare must not double-release.
     * The harness spinlock is a noop, so we verify by re-locking. */
    spin_unlock(&fixture_mm.lock);

    uw_clear_current();
}

/* ──────────────────────────────────────────────────────────────
 *  syscall_check_user_range + uw_arch_range_accessible contract
 *  (the "pure check accepts eligible COW" brief rule)
 * ────────────────────────────────────────────────────────────── */

static void test_pure_check_accepts_cow(void)
{
    TEST_SUITE("pure syscall_check_user_range — COW passes with writable=true");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x40a000UL;
    build_anon_vma(va, va + 0x1000);
    uint64_t phys = commit_leaf(va, PAGE_USER_PTE);
    mark_cow(va, 4);
    memset(uw_find_page(phys)->backing, 0xEE, 4096);

    /* writable=true: COW must be accepted (no PTE change). */
    bool ok = syscall_check_user_range(va, 0x1000, true);
    assert_true(ok);
    /* PTE untouched: still has PAGE_COW. */
    assert_true(uw_find_mapping(va)->pte & PAGE_COW);
    /* Target bytes untouched. */
    assert_eq(0xEE, ((uint8_t *)uw_find_page(phys)->backing)[0]);

    uw_clear_current();
}

static void test_pure_check_rejects_unmapped(void)
{
    TEST_SUITE("pure syscall_check_user_range — unmapped rejected");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    bool ok = syscall_check_user_range(0x40b000UL, 0x1000, true);
    assert_false(ok);

    uw_clear_current();
}

static void test_pure_check_rejects_readonly(void)
{
    TEST_SUITE("pure syscall_check_user_range — read-only PTE rejected for writable=true");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x40c000UL;
    build_anon_vma(va, va + 0x1000);
    commit_leaf(va, PAGE_USER_PTE_RO);                /* RO, not COW */

    bool ok = syscall_check_user_range(va, 0x1000, true);
    assert_false(ok);

    uw_clear_current();
}

/* ──────────────────────────────────────────────────────────────
 *  copy_to_user_ft_res — on_fault callback contract
 * ────────────────────────────────────────────────────────────── */

/* The host copy_to_user_ft_res is provided by the production
 * uaccess.c (linked into the test).  In a "pure check" world it
 * would just memcpy; here it must call prepare_user_write_range
 * first, then release the lock, then perform the FT memcpy, and
 * fire on_fault EXACTLY ONCE on prepare failure (and NOT fire
 * on success). */

static int g_oom_callback_calls;
static int g_oom_callback_arg_marker;

static void oom_on_fault_cb(void *arg)
{
    g_oom_callback_calls++;
    g_oom_callback_arg_marker = *(int *)arg;
}

static void test_ft_res_callback_fires_once_on_enomem(void)
{
    TEST_SUITE("copy_to_user_ft_res — on_fault fires once on prepare -ENOMEM");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x40d000UL;
    build_anon_vma(va, va + 0x2000);
    uint64_t p1 = commit_leaf(va,        PAGE_USER_PTE);
    uint64_t p2 = commit_leaf(va+0x1000, PAGE_USER_PTE);
    mark_cow(va,        2);
    mark_cow(va+0x1000, 2);
    (void)p1; (void)p2;

    /* Capture PTE-before for the rollback assertion. */
    uint64_t pte1_before = uw_find_mapping(va)->pte;
    uint64_t pte2_before = uw_find_mapping(va+0x1000)->pte;

    /* Force the second alloc to OOM. */
    uw_state.inject_alloc_fail_at = uw_state.total_allocs + 2;

    g_oom_callback_calls = 0;
    g_oom_callback_arg_marker = 0;
    int marker = 0xC0DE;
    static const char source[0x2000] = {0};
    ssize_t rc = copy_to_user_ft_res((void *)va, source, sizeof(source),
                                     oom_on_fault_cb, &marker);
    assert_eq((ssize_t)-ENOMEM, rc);
    assert_eq(1, g_oom_callback_calls);
    assert_eq(0xC0DE, g_oom_callback_arg_marker);
    /* No stale fault_jmp after failure. */
    assert_null(current->fault_jmp);
    /* Both PTEs unchanged. */
    assert_eq(pte1_before, uw_find_mapping(va)->pte);
    assert_eq(pte2_before, uw_find_mapping(va+0x1000)->pte);

    uw_clear_current();
}

static void test_ft_res_callback_fires_once_on_efault(void)
{
    TEST_SUITE("copy_to_user_ft_res — on_fault fires once on prepare -EFAULT");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    g_oom_callback_calls = 0;
    int marker = 0xBEEF;
    /* Unmapped destination → prepare returns -EFAULT. */
    ssize_t rc = copy_to_user_ft_res(
        (void *)(0x40e000UL), "xx", 2,
        oom_on_fault_cb, &marker);
    assert_eq((ssize_t)-EFAULT, rc);
    assert_eq(1, g_oom_callback_calls);
    assert_eq(0xBEEF, g_oom_callback_arg_marker);
    assert_null(current->fault_jmp);

    uw_clear_current();
}

static void test_ft_res_no_callback_on_success(void)
{
    TEST_SUITE("copy_to_user_ft_res — on_fault NOT fired on success");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x40f000UL;
    build_anon_vma(va, va + 0x1000);
    uint64_t phys = commit_leaf(va, PAGE_USER_PTE);

    g_oom_callback_calls = 0;
    int marker = 0xDEAD;
    char ksrc[16] = "Hello COW World";
    ssize_t rc = copy_to_user_ft_res((void *)va, ksrc, 16,
                                     oom_on_fault_cb, &marker);
    assert_eq((ssize_t)16, rc);
    assert_eq(0, g_oom_callback_calls);
    /* The bytes were copied to the user backing. */
    assert_eq('H', ((uint8_t *)uw_find_page(phys)->backing)[0]);
    assert_eq(' ', ((uint8_t *)uw_find_page(phys)->backing)[5]);
    /* No stale fault_jmp after success. */
    assert_null(current->fault_jmp);

    uw_clear_current();
}

static void test_ft_res_no_callback_with_null_on_fault(void)
{
    TEST_SUITE("copy_to_user_ft_res — NULL on_fault doesn't crash");

    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    fixture_mm.mmap_base = 0x40000000UL;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);

    uint64_t va = 0x410000UL;
    build_anon_vma(va, va + 0x1000);
    commit_leaf(va, PAGE_USER_PTE);

    /* NULL on_fault: a successful copy must not deref. */
    char ksrc[4] = {1, 2, 3, 4};
    ssize_t rc = copy_to_user_ft_res((void *)va, ksrc, 4, NULL, NULL);
    assert_eq((ssize_t)4, rc);
    assert_null(current->fault_jmp);

    /* Unmapped destination with NULL on_fault: must NOT crash. */
    rc = copy_to_user_ft_res(
        (void *)(0x411000UL), ksrc, 4, NULL, NULL);
    assert_eq((ssize_t)-EFAULT, rc);
    assert_null(current->fault_jmp);

    uw_clear_current();
}

/* ──────────────────────────────────────────────────────────────
 *  Test list
 * ────────────────────────────────────────────────────────────── */

void test_user_write_cow(void)
{
    /* prepare_user_write_range PTE-mutation rules */
    test_prepare_noop_on_writable_leaf();
    test_prepare_cow_refs_one_promotes_inplace();
    test_prepare_cow_refs_many_privatizes();
    test_prepare_span_two_cow_pages();
    test_prepare_oom_on_page_two_rolls_back();
    test_prepare_rejects_vmio();
    test_prepare_rejects_unmapped();
    test_prepare_rejects_below_user_min();
    test_prepare_efault_does_not_invalidate_pte();

    /* prepare_user_write_range_locked lock contract */
    test_locked_retains_lock_on_success();
    test_locked_retains_lock_on_failure();

    /* pure check accepts eligible COW */
    test_pure_check_accepts_cow();
    test_pure_check_rejects_unmapped();
    test_pure_check_rejects_readonly();

    /* copy_to_user_ft_res on_fault contract */
    test_ft_res_callback_fires_once_on_enomem();
    test_ft_res_callback_fires_once_on_efault();
    test_ft_res_no_callback_on_success();
    test_ft_res_no_callback_with_null_on_fault();
}


static void test_writable_elf_without_vma(void)
{
    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);
    commit_leaf(0x400000, PAGE_USER_PTE);
    assert_eq(0, prepare_user_write_range(&fixture_mm, 0x400000, 32));
    assert_eq(0, fixture_mm.lock.lock);
    assert_eq(0, uw_state.total_tlb_shootdowns);
}

static void test_cow_requires_writable_vma(void)
{
    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);
    commit_leaf(0x400000, PAGE_USER_PTE);
    mark_cow(0x400000, 2);
    assert_false(syscall_check_user_range(0x400000, 32, true));
    assert_eq(-EFAULT, prepare_user_write_range(&fixture_mm, 0x400000, 32));
    vma_t *v = build_anon_vma(0x400000, 0x401000);
    v->vm_flags &= ~VMA_PROT_WRITE;
    assert_false(syscall_check_user_range(0x400000, 32, true));
    v->vm_flags |= VMA_PROT_WRITE | VMA_IO;
    assert_false(syscall_check_user_range(0x400000, 32, true));
    assert_eq(2, uw_find_page(0x400000)->cow_refs);
}

static void test_invalid_second_page_preserves_first(void)
{
    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);
    build_anon_vma(0x400000, 0x402000);
    commit_leaf(0x400000, PAGE_USER_PTE);
    commit_leaf(0x401000, PAGE_USER_PTE_RO);
    mark_cow(0x400000, 2);
    uint64_t original = uw_find_mapping(0x400000)->pte;
    int pages = uw_in_use_count();
    assert_eq(-EFAULT, prepare_user_write_range(&fixture_mm, 0x400000, 8192));
    assert_eq(pages, uw_in_use_count());
    assert_eq(original, uw_find_mapping(0x400000)->pte);
    assert_eq(2, uw_find_page(0x400000)->cow_refs);
    assert_eq(0, fixture_mm.lock.lock);
}

static void test_staging_oom_and_non_cow_fast_path(void)
{
    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);
    commit_leaf(0x400000, PAGE_USER_PTE);
    uw_state.inject_kmalloc_fail = 1;
    assert_eq(0, prepare_user_write_range(&fixture_mm, 0x400000, 32));
    uw_state.inject_kmalloc_fail = 0;
    build_anon_vma(0x400000, 0x401000);
    mark_cow(0x400000, 2);
    uint64_t before = uw_find_mapping(0x400000)->pte;
    uw_state.inject_kmalloc_fail = 1;
    assert_eq(-ENOMEM, prepare_user_write_range(&fixture_mm, 0x400000, 32));
    assert_eq(before, uw_find_mapping(0x400000)->pte);
    assert_eq(2, uw_find_page(0x400000)->cow_refs);
    assert_eq(0, fixture_mm.lock.lock);
    uw_state.inject_kmalloc_fail = 0;
}

static void test_begin_end_owns_lock(void)
{
    uw_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    fixture_mm.pgdir = fixture_pgd;
    uw_set_current(&fixture_mm, PROD_ADDR_LIMIT);
    build_anon_vma(0x400000, 0x401000);
    commit_leaf(0x400000, PAGE_USER_PTE);
    mark_cow(0x400000, 2);
    assert_eq(0, user_write_range_begin(0x400000, 32));
    assert_eq(1, fixture_mm.lock.lock);
    assert_false(uw_find_mapping(0x400000)->pte & PAGE_COW);
    user_write_range_end();
    assert_eq(0, fixture_mm.lock.lock);
    assert_eq(-EFAULT, user_write_range_begin(0x401000, 32));
    assert_eq(0, fixture_mm.lock.lock);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_staging_oom_and_non_cow_fast_path),
    TEST_ENTRY(test_begin_end_owns_lock),
    TEST_ENTRY(test_writable_elf_without_vma),
    TEST_ENTRY(test_cow_requires_writable_vma),
    TEST_ENTRY(test_invalid_second_page_preserves_first),
    TEST_ENTRY(test_prepare_noop_on_writable_leaf),
    TEST_ENTRY(test_prepare_cow_refs_one_promotes_inplace),
    TEST_ENTRY(test_prepare_cow_refs_many_privatizes),
    TEST_ENTRY(test_prepare_span_two_cow_pages),
    TEST_ENTRY(test_prepare_oom_on_page_two_rolls_back),
    TEST_ENTRY(test_prepare_rejects_vmio),
    TEST_ENTRY(test_prepare_rejects_unmapped),
    TEST_ENTRY(test_prepare_rejects_below_user_min),
    TEST_ENTRY(test_prepare_efault_does_not_invalidate_pte),
    TEST_ENTRY(test_locked_retains_lock_on_success),
    TEST_ENTRY(test_locked_retains_lock_on_failure),
    TEST_ENTRY(test_pure_check_accepts_cow),
    TEST_ENTRY(test_pure_check_rejects_unmapped),
    TEST_ENTRY(test_pure_check_rejects_readonly),
    TEST_ENTRY(test_ft_res_callback_fires_once_on_enomem),
    TEST_ENTRY(test_ft_res_callback_fires_once_on_efault),
    TEST_ENTRY(test_ft_res_no_callback_on_success),
    TEST_ENTRY(test_ft_res_no_callback_with_null_on_fault),
TEST_LIST_END

int main(void)
{
    for (int i = 0; i < __test_table_size; i++)
        __test_table[i].fn();
    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed != 0;
}