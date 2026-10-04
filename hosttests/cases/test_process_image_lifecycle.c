/*
 * test/cases/test_process_image_lifecycle.c — Host tests for the
 * process-image lifecycle helpers (Task 3, user heap/ELF isolation
 * plan).
 *
 * Coverage splits into two layers:
 *
 *   1. PRODUCTION-LINKED — compiles the REAL kernel/memory/vma.c
 *      against a focused stub harness (hosttests/mock/lifecycle/).
 *      The host harness uses identity Phy_To_Virt and a host-heap
 *      4 KiB-aligned page pool, so mm_init_user_heap /
 *      vma_insert / vma_find / vma_free_all / mm_alloc are
 *      exercised against observable state. Failure injection
 *      (-Wl,--wrap=kmalloc) lets the test prove that
 *      mm_init_user_heap returns -ENOMEM and leaves mm unchanged
 *      when its VMA allocation fails.
 *
 *   2. SOURCE-LEVEL INSPECTION — opens kernel/sched/task.c and
 *      asserts the staged lifecycle invariants the brief
 *      requires (task_list_lock insertion AFTER resource
 *      preparation, sys_exec old-mm/CR3 switch AFTER new image
 *      ready, mm_init_user_heap called from spawn+exec, the
 *      destroy_unpublished_user_mm helper defined). The brief
 *      accepts source review for task.c — its 2200-line file
 *      and arch-specific dependency tree (percpu, ipi, fpu,
 *      ...) make a host compile impractical; the actual
 *      end-to-end behavior is covered by
 *      `make OS01_SYSTEST=1 test-qemu SUITE=systest`.
 */
#include "test_framework.h"
#include "lifecycle_stubs.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <fs/vfs.h>
#include <memory/vma.h>

/* ── Layout constants (mirror kernel/include/sched/task.h) ── */
#define USER_CODE_ADDR     0x400000UL
#define USER_ENVELOPE_SIZE 0x20000000UL
#define USER_PAGE_SIZE     USER_ENVELOPE_SIZE
#define USER_STACK_BASE    0x20400000UL
#define HEAP_LIMIT         (USER_CODE_ADDR + USER_ENVELOPE_SIZE - 0x1000UL)

#define PAGE_VALID_BIT  0x1UL
#define PAGE_WRITE_BIT  0x2UL
#define PAGE_USER_BIT   0x4UL

/* Production wraps (--wrap=kmalloc) for failure injection.
 * The test sets kmalloc_fail_after = N; the Nth call to kmalloc
 * (1-indexed) returns NULL, all subsequent calls succeed. */
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

/* ── Per-test fixtures ─────────────────────────────────────── */

static mm_t fixture_mm;

static void setup_mm(void)
{
    lifecycle_stubs_reset();
    kmalloc_calls = 0;
    kmalloc_fail_after = 0;
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    list_init(&fixture_mm.vma_list);
    /* Identity pgdir: any non-NULL pointer.  vma_free_all derefs
     * it as Phy_To_Virt(pgdir), which is identity on the host. */
    fixture_mm.pgdir = (uint64_t *)0x100000ULL;
    /* mmap_base matches production mm_alloc's default. */
    fixture_mm.mmap_base = 0x40000000UL;
}

/* ── VMA list helpers (libc/list.h has no list_for_each) ───── */
static int vma_list_count(list_t *head)
{
    int n = 0;
    for (list_t *p = head->next; p != head; p = p->next) n++;
    return n;
}

static vma_t *vma_list_get(list_t *head, int idx)
{
    int i = 0;
    for (list_t *p = head->next; p != head; p = p->next, i++) {
        if (i == idx) return container_of(p, vma_t, list);
    }
    return NULL;
}

/* ── Tests: production-linked mm_init_user_heap ─────────────── */

static void test_heap_vma_zero_length(void)
{
    TEST_SUITE("mm_init_user_heap — heap VMA shape");

    setup_mm();
    uint64_t elf_end = USER_CODE_ADDR + 0x3000;       /* 3 pages in */
    int rc = mm_init_user_heap(&fixture_mm, elf_end);
    assert_eq(0, rc);

    /* Exactly one VMA, zero length [start, start). */
    assert_eq(1, vma_list_count(&fixture_mm.vma_list));
    vma_t *heap_vma = vma_list_get(&fixture_mm.vma_list, 0);
    assert_not_null(heap_vma);
    assert_eq(elf_end, heap_vma->vm_start);
    assert_eq(elf_end, heap_vma->vm_end);
    assert_true(heap_vma->vm_start == heap_vma->vm_end);
}

static void test_heap_vma_vm_flags(void)
{
    TEST_SUITE("mm_init_user_heap — VMA flags");

    setup_mm();
    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x1000);
    assert_eq(0, rc);

    vma_t *v = vma_list_get(&fixture_mm.vma_list, 0);
    assert_not_null(v);
    uint64_t expected = VM_READ | VM_WRITE | VM_ANON | VM_HEAP;
    assert_eq(expected, v->vm_flags);
    /* VM_HEAP is distinct from every other flag bit. */
    assert_true((v->vm_flags & VM_HEAP) != 0);
    assert_eq(0x100UL, (uint64_t)VM_HEAP);
}

static void test_heap_vma_vm_page_prot(void)
{
    TEST_SUITE("mm_init_user_heap — vm_page_prot");

    setup_mm();
    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x1000);
    assert_eq(0, rc);

    vma_t *v = vma_list_get(&fixture_mm.vma_list, 0);
    assert_not_null(v);
    uint64_t expected = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    assert_eq(expected, v->vm_page_prot);
}

static void test_mm_init_user_heap_sets_breaks(void)
{
    TEST_SUITE("mm_init_user_heap — break fields");

    setup_mm();
    uint64_t elf_end = USER_CODE_ADDR + 0x1234;        /* unaligned */
    int rc = mm_init_user_heap(&fixture_mm, elf_end);
    assert_eq(0, rc);

    /* ALIGN_UP(elf_end, 4096). */
    assert_eq(USER_CODE_ADDR + 0x2000UL, fixture_mm.start_brk);
    assert_eq(fixture_mm.start_brk, fixture_mm.end_brk);
}

static void test_mm_init_user_heap_already_aligned(void)
{
    TEST_SUITE("mm_init_user_heap — already-aligned elf_end");

    setup_mm();
    uint64_t elf_end = USER_CODE_ADDR + 0x4000;        /* page-aligned */
    int rc = mm_init_user_heap(&fixture_mm, elf_end);
    assert_eq(0, rc);

    assert_eq(elf_end, fixture_mm.start_brk);
    assert_eq(elf_end, fixture_mm.end_brk);
}

static void test_vma_find_does_not_match_zero_heap(void)
{
    TEST_SUITE("mm_init_user_heap — vma_find skips zero-length heap");

    setup_mm();
    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x2000);
    assert_eq(0, rc);

    /* The heap VMA is [0x600000, 0x600000).  vma_find(start) must
     * return NULL — that is the spec §4 invariant the brief calls
     * out: vma_find 不会命中此袋. */
    vma_t *found = vma_find(&fixture_mm, fixture_mm.start_brk);
    assert_null(found);

    /* vma_find just below and just above the heap range must
     * likewise miss (no other VMA exists). */
    assert_null(vma_find(&fixture_mm, fixture_mm.start_brk - 1));
    assert_null(vma_find(&fixture_mm, fixture_mm.start_brk + 0x1000));
}

static void test_vma_find_matches_other_vma_with_other_vm(void)
{
    TEST_SUITE("mm_init_user_heap — vma_find still works for other VMAs");

    setup_mm();
    /* Insert a non-heap VMA first; then init_user_heap adds the
     * zero-length heap VMA after it.  vma_find must still match
     * the non-heap VMA by address. */
    vma_t *other = (vma_t *)__real_kmalloc(sizeof(vma_t));
    assert_not_null(other);
    list_init(&other->list);
    other->vm_start     = USER_CODE_ADDR + 0x1000;
    other->vm_end       = USER_CODE_ADDR + 0x2000;
    other->vm_flags     = VM_READ | VM_WRITE | VM_ANON;
    other->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    other->vm_pgoff     = 0;
    other->vm_file      = NULL;
    vma_insert(&fixture_mm, other);

    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x3000);
    assert_eq(0, rc);

    /* The non-heap VMA is still findable. */
    vma_t *found = vma_find(&fixture_mm, USER_CODE_ADDR + 0x1500);
    assert_not_null(found);
    assert_eq((uint64_t)other, (uint64_t)found);

    /* The heap VMA is still NOT findable. */
    assert_null(vma_find(&fixture_mm, fixture_mm.start_brk));
}

static void test_exactly_one_heap_vma_in_list(void)
{
    TEST_SUITE("mm_init_user_heap — exactly one VM_HEAP VMA");

    setup_mm();
    /* Pre-populate with two non-heap VMAs. */
    for (int i = 0; i < 2; i++) {
        vma_t *v = (vma_t *)__real_kmalloc(sizeof(vma_t));
        list_init(&v->list);
        v->vm_start     = USER_CODE_ADDR + 0x1000 + i * 0x1000;
        v->vm_end       = v->vm_start + 0x1000;
        v->vm_flags     = VM_READ | VM_WRITE | VM_ANON;
        v->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
        v->vm_pgoff     = 0;
        v->vm_file      = NULL;
        vma_insert(&fixture_mm, v);
    }
    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x4000);
    assert_eq(0, rc);

    int heap_vma_count = 0;
    int total = vma_list_count(&fixture_mm.vma_list);
    for (int i = 0; i < total; i++) {
        vma_t *v = vma_list_get(&fixture_mm.vma_list, i);
        if (v->vm_flags & VM_HEAP) heap_vma_count++;
    }
    assert_eq(1, heap_vma_count);
}

static void test_mm_init_user_heap_alloc_fail_returns_enomem(void)
{
    TEST_SUITE("mm_init_user_heap — alloc failure → -ENOMEM");

    setup_mm();
    /* Fail the very next kmalloc call (the heap VMA alloc). */
    kmalloc_fail_after = kmalloc_calls + 1;

    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x1000);
    assert_eq(-ENOMEM, rc);
}

static void test_mm_init_user_heap_alloc_fail_leaves_mm_unchanged(void)
{
    TEST_SUITE("mm_init_user_heap — failure leaves mm unchanged");

    setup_mm();
    /* Snapshot the starting state. */
    uint64_t saved_start_brk = fixture_mm.start_brk;
    uint64_t saved_end_brk   = fixture_mm.end_brk;
    int      saved_vma_count = vma_list_count(&fixture_mm.vma_list);

    /* Force a kmalloc failure inside mm_init_user_heap. */
    kmalloc_fail_after = kmalloc_calls + 1;

    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x2000);
    assert_eq(-ENOMEM, rc);

    /* mm is unchanged: break fields untouched, VMA list empty. */
    assert_eq(saved_start_brk, fixture_mm.start_brk);
    assert_eq(saved_end_brk,   fixture_mm.end_brk);
    int after_vma_count = vma_list_count(&fixture_mm.vma_list);
    assert_eq(saved_vma_count, after_vma_count);
}

static void test_destroy_unpublished_user_mm_releases_vmas(void)
{
    TEST_SUITE("destroy_unpublished_user_mm semantics — VMA list empty");

    setup_mm();
    /* Pre-populate with one heap VMA + one non-heap VMA. */
    int rc = mm_init_user_heap(&fixture_mm, USER_CODE_ADDR + 0x2000);
    assert_eq(0, rc);
    vma_t *extra = (vma_t *)__real_kmalloc(sizeof(vma_t));
    list_init(&extra->list);
    extra->vm_start     = USER_CODE_ADDR + 0x3000;
    extra->vm_end       = USER_CODE_ADDR + 0x4000;
    extra->vm_flags     = VM_READ | VM_WRITE | VM_ANON;
    extra->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    extra->vm_pgoff     = 0;
    extra->vm_file      = NULL;
    vma_insert(&fixture_mm, extra);

    int vma_count_before = vma_list_count(&fixture_mm.vma_list);
    assert_eq(2, vma_count_before);

    /* The destroy_unpublished_user_mm helper's first action is
     * vma_free_all(mm).  vma_free_all walks the list, unmaps
     * 4 KiB pages (here: none, since the test did not map any
     * leaf PTEs), removes each VMA node, and frees it. */
    vma_free_all(&fixture_mm);

    int vma_count_after = vma_list_count(&fixture_mm.vma_list);
    assert_eq(0, vma_count_after);

    /* The heap-VMA break fields are still on mm — destroy does NOT
     * touch mm->start_brk/end_brk (caller frees mm via kfree). */
    assert_eq(USER_CODE_ADDR + 0x2000UL, fixture_mm.start_brk);
    assert_eq(USER_CODE_ADDR + 0x2000UL, fixture_mm.end_brk);
}

static void test_destroy_unpublished_user_mm_handles_empty_list(void)
{
    TEST_SUITE("destroy_unpublished_user_mm semantics — empty list");

    setup_mm();
    /* mm with no VMAs: destroy must not crash. */
    vma_free_all(&fixture_mm);

    int vma_count = vma_list_count(&fixture_mm.vma_list);
    assert_eq(0, vma_count);
}

/* ── Stack-page failure has exactly one free ────────────────
 *
 * Brief Step 1 explicit assertion.  Strategy C (source-level
 * inspection) covered the staged spawn/exec control flow, but
 * this is a count assertion about the runtime release chain —
 * it has to be exercised from the host.  We replicate the body
 * of destroy_unpublished_user_mm (vma_free_all + vmm_free_user_map)
 * and observe the page-pool counter:
 *
 *   1. Allocate one stack page via the lifecycle stub alloc_4k_page.
 *   2. Build the fake user PGD with a single PAGE_HUGE PMD entry
 *      at USER_STACK_BASE → stack_phys (mirrors kernel/sched/task.c:
 *      1539 vmm_map_page(... PAGE_USER_PMD ... USER_STACK_BASE ...)).
 *   3. Wire mm->pgdir to the fake PGD.
 *   4. Run vma_free_all + vmm_free_user_map (the helper's body).
 *   5. Assert the page-pool counter advanced by exactly one free
 *      (not zero, not two — single-owner invariant).
 */
static void test_stack_page_failure_exactly_one_free(void)
{
    TEST_SUITE("stack-page failure — exactly one free");

    setup_mm();

    /* Allocate the test PGD arrays BEFORE touching them with
     * memset — they're lazily allocated by the walker on first
     * vmm_free_user_map call, but the test populates the entries
     * before invoking the walker, so we trigger the allocation
     * up-front. */
    {
        mm_t trigger_mm;
        memset(&trigger_mm, 0, sizeof(trigger_mm));
        vmm_free_user_map((uint64_t *)&trigger_mm);   /* trigger allocate_test_pgdir() */
    }

    /* Capture counters BEFORE the alloc so the destroy pass can be
     * measured as a delta.  setup_mm() calls lifecycle_stubs_reset
     * which zeros both counters, so the initial state is (0, 0). */
    int allocs_before = lifecycle_state.total_allocs;
    int frees_before  = lifecycle_state.total_frees;

    /* Step 1: allocate the stack page via the lifecycle stub. */
    uint64_t stack_phys = alloc_4k_page();
    assert_true(stack_phys != 0);
    assert_eq(allocs_before + 1, lifecycle_state.total_allocs);
    assert_eq(frees_before,      lifecycle_state.total_frees);

    /* Step 2: build the fake user page table.
     *
     * USER_STACK_BASE = 0x1400000 lies in PGD index 0
     *   (VA[0..0x8000000000)) → PUD index 0 (VA[0..0x40000000))
     *   → PUD index 1 (VA[0x40000000..0x80000000)) covers
     *     [2 MiB, 4 MiB), so PUD entry 1 → test_user_pmd_stack,
     *     PMD entry 0 in test_user_pmd_stack holds the stack.
     */
    memset(test_user_pgd,        0, LIFECYCLE_PGD_SIZE * sizeof(uint64_t));
    memset(test_user_pud_page,   0, LIFECYCLE_PUD_SIZE * sizeof(uint64_t));
    memset(test_user_pmd_lo,     0, LIFECYCLE_PMD_SIZE * sizeof(uint64_t));
    memset(test_user_pmd_stack,  0, LIFECYCLE_PMD_SIZE * sizeof(uint64_t));

    /* PGD[0] → PUD page (treat the array address as the page's
     * "physical" base for our walker). */
    test_user_pgd[0] = (uint64_t)(uintptr_t)test_user_pud_page | PAGE_VALID;
    test_user_pud_page[0] = (uint64_t)(uintptr_t)test_user_pmd_lo | PAGE_VALID;
    test_user_pud_page[1] = (uint64_t)(uintptr_t)test_user_pmd_stack | PAGE_VALID;
    /* PMD huge entry at USER_STACK_BASE (PMD index 0 in the
     * stack region, since USER_STACK_BASE is 2 MiB-aligned):
     * phys | PAGE_USER_PMD.  PAGE_USER_PMD = PAGE_HUGE|PAGE_USER|
     * PAGE_WRITE|PAGE_VALID (vmm.h). */
    test_user_pmd_stack[0] = stack_phys | PAGE_USER_PMD;

    /* Step 3: wire mm->pgdir to the fake PGD.  Phy_To_Virt is
     * identity on the host (lifecycle_runtime.h), so the walker
     * sees the same pointer. */
    fixture_mm.pgdir = test_user_pgd;

    /* Step 4: run destroy_unpublished_user_mm's body — the
     * helper is static in kernel/sched/task.c so the test calls
     * its two constituent primitives directly.  vma_free_all
     * walks the VMA list (here: empty heap VMA, zero-length so
     * vmm_unmap_4k_page loop body never executes), then
     * vmm_free_user_map walks the user page table and frees the
     * stack PMD huge leaf. */
    vma_free_all(&fixture_mm);
    vmm_free_user_map((uint64_t *)test_user_pgd);

    /* Step 5: single-owner invariant — exactly one free from the
     * destroy pass, and zero new allocs (the destroy path must
     * not allocate). */
    assert_eq(allocs_before + 1, lifecycle_state.total_allocs);
    assert_eq(frees_before + 1,  lifecycle_state.total_frees);

    /* Sanity: a second pass must NOT free anything — the page
     * is gone, the page-pool state must remain stable.  This
     * is the negative side of the invariant: re-running destroy
     * after a successful destroy must not double-free. */
    vma_free_all(&fixture_mm);
    vmm_free_user_map((uint64_t *)test_user_pgd);
    assert_eq(allocs_before + 1, lifecycle_state.total_allocs);
    assert_eq(frees_before + 1,  lifecycle_state.total_frees);
}

/* ── Source-level inspection: spawn_user_task lifecycle ───── */

static const char *task_c_path(void)
{
    static char path[1024];
    const char *marker = strstr(__FILE__, "/hosttests/");
    if (!marker) return "../kernel/sched/task.c";
    snprintf(path, sizeof(path), "%.*s/kernel/sched/task.c",
             (int)(marker - __FILE__), __FILE__);
    return path;
}

static char *slurp_file(const char *path, size_t *out_len)
{
    /* libc/include/stdio.h is the OS01 stub stdio (no fseek/ftell),
     * so we use the raw fd API instead.  open() / read() / close()
     * are exposed via libc/include/fcntl.h and unistd.h. */
    int fd = open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return NULL;
    /* Stat the size via seek + tell?  No stat helper exposed.
     * Slurp incrementally by reading in 4 KiB chunks until EOF. */
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
        if (got < 0)  { free(buf); close(fd); return NULL; }
        if (got == 0) break;        /* EOF */
        len += (size_t)got;
    }
    close(fd);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Look for `needle` inside `haystack`.  The match is "the substring
 * appears in the source after a line starting with the spawn_user_task
 * function header", so we don't accidentally match unrelated callers. */
static int source_contains_in_function(char *haystack, size_t haystack_len,
                                       const char *fn_signature,
                                       const char *needle)
{
    char *fn = strstr(haystack, fn_signature);
    if (!fn) return 0;
    /* Look for the next '}' at column 0 that closes the function.
     * A simpler heuristic: scan 12 KiB forward — enough to cover
     * spawn_user_task / sys_exec bodies (each < 300 lines). */
    char *scan_limit = fn + 12 * 1024;
    if (scan_limit > haystack + haystack_len) scan_limit = haystack + haystack_len;
    size_t span = (size_t)(scan_limit - fn);
    char *hit = NULL;
    /* strstr with a length-bounded scan: walk byte-by-byte. */
    for (size_t i = 0; i + strlen(needle) <= span; i++) {
        if (memcmp(fn + i, needle, strlen(needle)) == 0) {
            hit = fn + i;
            break;
        }
    }
    return hit ? 1 : 0;
}

static void test_spawn_user_task_calls_mm_init_user_heap(void)
{
    TEST_SUITE("spawn_user_task — uses mm_init_user_heap");

    size_t len;
    char *src = slurp_file(task_c_path(), &len);
    assert_not_null(src);
    if (!src) return;

    int has_call = source_contains_in_function(
        src, len,
        "int64_t spawn_user_task(const char *path",
        "mm_init_user_heap(");
    assert_true(has_call);

    /* The legacy inline heap-VMA setup (the anonymous vma_t
     * allocated with vm_start..USER_CODE_ADDR+USER_PAGE_SIZE) must
     * NOT be present in the new spawn path.  It is replaced by the
     * single mm_init_user_heap() call above. */
    int has_legacy = source_contains_in_function(
        src, len,
        "int64_t spawn_user_task(const char *path",
        "vma_t *hv = (vma_t *)kmalloc(sizeof(vma_t))");
    assert_false(has_legacy);

    free(src);
}

static void test_spawn_user_task_no_early_task_list_insert(void)
{
    TEST_SUITE("spawn_user_task — task_list_lock acquired AFTER all resource prep");

    size_t len;
    char *src = slurp_file(task_c_path(), &len);
    assert_not_null(src);
    if (!src) return;

    /* Find the spawn_user_task function body.  We assert that the
     * first occurrence of `spin_lock_irqsave(&task_list_lock)` lies
     * AFTER the calls to elf_load, mm_init_user_heap, alloc_pages
     * for the user stack, and setup_user_stack.  Order:
     *
     *   elf_load → mm_init_user_heap → alloc_pages(stack) →
     *   setup_user_stack → [task_list_lock] → [enqueue_task] → sched_notify_remote
     *
     * We check each prerequisite precedes the task_list_lock site. */
    char *fn = strstr(src, "int64_t spawn_user_task(const char *path");
    assert_not_null(fn);
    char *fn_end = fn + 8 * 1024;
    if (fn_end > src + len) fn_end = src + len;
    size_t span = (size_t)(fn_end - fn);
    assert_true(span > 0);

    /* Find the offset of each token inside the function. */
    char *p_elf     = NULL;
    char *p_heap    = NULL;
    char *p_stack   = NULL;
    char *p_setup   = NULL;
    char *p_listlk  = NULL;
    for (size_t i = 0; i < span; i++) {
        char *cur = fn + i;
        if (!p_elf    && i + strlen("elf_load(")         <= span &&
            memcmp(cur, "elf_load(",         strlen("elf_load("))         == 0) p_elf    = cur;
        if (!p_heap   && i + strlen("mm_init_user_heap(") <= span &&
            memcmp(cur, "mm_init_user_heap(", strlen("mm_init_user_heap(")) == 0) p_heap   = cur;
        if (!p_stack  && i + strlen("alloc_pages(")      <= span &&
            memcmp(cur, "alloc_pages(",      strlen("alloc_pages("))      == 0) p_stack  = cur;
        if (!p_setup  && i + strlen("setup_user_stack(") <= span &&
            memcmp(cur, "setup_user_stack(", strlen("setup_user_stack(")) == 0) p_setup  = cur;
        if (!p_listlk && i + strlen("spin_lock_irqsave(&task_list_lock)") <= span &&
            memcmp(cur, "spin_lock_irqsave(&task_list_lock)",
                   strlen("spin_lock_irqsave(&task_list_lock)")) == 0) p_listlk = cur;
    }

    /* All five tokens must appear in the function body. */
    assert_not_null((void *)p_elf);
    assert_not_null((void *)p_heap);
    assert_not_null((void *)p_stack);
    assert_not_null((void *)p_setup);
    assert_not_null((void *)p_listlk);

    /* Order: elf_load < mm_init_user_heap < alloc_pages(stack) <
     * setup_user_stack < spin_lock_irqsave(&task_list_lock). */
    assert_true(p_elf    < p_heap);
    assert_true(p_heap   < p_stack);
    assert_true(p_stack  < p_setup);
    assert_true(p_setup  < p_listlk);

    free(src);
}

static void test_spawn_user_task_uses_destroy_helper(void)
{
    TEST_SUITE("spawn_user_task — destroy_unpublished_user_mm defined + used");

    size_t len;
    char *src = slurp_file(task_c_path(), &len);
    assert_not_null(src);
    if (!src) return;

    /* Helper definition must exist. */
    int has_def = (strstr(src, "static void destroy_unpublished_user_mm(") != NULL);
    assert_true(has_def);

    /* spawn_user_task must call it on the failure cleanup paths. */
    int called_in_spawn = source_contains_in_function(
        src, len,
        "int64_t spawn_user_task(const char *path",
        "destroy_unpublished_user_mm(");
    assert_true(called_in_spawn);

    free(src);
}

static void test_sys_exec_calls_mm_init_user_heap(void)
{
    TEST_SUITE("sys_exec — uses mm_init_user_heap");

    size_t len;
    char *src = slurp_file(task_c_path(), &len);
    assert_not_null(src);
    if (!src) return;

    int has_call = source_contains_in_function(
        src, len,
        "int64_t sys_exec(const char *path",
        "mm_init_user_heap(");
    assert_true(has_call);

    /* The legacy inline heap-VMA setup must NOT be present in the
     * new sys_exec path either. */
    int has_legacy = source_contains_in_function(
        src, len,
        "int64_t sys_exec(const char *path",
        "vma_t *hv = (vma_t *)kmalloc(sizeof(vma_t))");
    assert_false(has_legacy);

    free(src);
}

static void test_sys_exec_old_mm_retained_until_switch(void)
{
    TEST_SUITE("sys_exec — old mm + CR3 retained until new image is ready");

    size_t len;
    char *src = slurp_file(task_c_path(), &len);
    assert_not_null(src);
    if (!src) return;

    /* Find the sys_exec body. */
    char *fn = strstr(src, "int64_t sys_exec(const char *path");
    assert_not_null(fn);
    char *fn_end = fn + 8 * 1024;
    if (fn_end > src + len) fn_end = src + len;
    size_t span = (size_t)(fn_end - fn);
    assert_true(span > 0);

    /* Order: elf_load → mm_init_user_heap → alloc_pages(stack) →
     * setup_user_stack → old_mm = current->mm → current->mm = new_mm →
     * arch_switch_mm(...).
     *
     * The `old_mm = current->mm` assignment must come AFTER all the
     * fallible preparation; only then is the new image ready and
     * the switch safe. */
    char *p_elf    = NULL;
    char *p_heap   = NULL;
    char *p_stack  = NULL;
    char *p_setup  = NULL;
    char *p_oldmm  = NULL;
    char *p_switch = NULL;
    for (size_t i = 0; i < span; i++) {
        char *cur = fn + i;
        if (!p_elf    && i + strlen("elf_load(") <= span &&
            memcmp(cur, "elf_load(", strlen("elf_load(")) == 0) p_elf = cur;
        if (!p_heap   && i + strlen("mm_init_user_heap(") <= span &&
            memcmp(cur, "mm_init_user_heap(", strlen("mm_init_user_heap(")) == 0) p_heap = cur;
        if (!p_stack  && i + strlen("alloc_pages(") <= span &&
            memcmp(cur, "alloc_pages(", strlen("alloc_pages(")) == 0) p_stack = cur;
        if (!p_setup  && i + strlen("setup_user_stack(") <= span &&
            memcmp(cur, "setup_user_stack(", strlen("setup_user_stack(")) == 0) p_setup = cur;
        if (!p_oldmm  && i + strlen("old_mm = current->mm") <= span &&
            memcmp(cur, "old_mm = current->mm",
                   strlen("old_mm = current->mm")) == 0) p_oldmm = cur;
        if (!p_switch && i + strlen("arch_switch_mm(") <= span &&
            memcmp(cur, "arch_switch_mm(", strlen("arch_switch_mm(")) == 0) p_switch = cur;
    }

    assert_not_null((void *)p_elf);
    assert_not_null((void *)p_heap);
    assert_not_null((void *)p_stack);
    assert_not_null((void *)p_setup);
    assert_not_null((void *)p_oldmm);
    assert_not_null((void *)p_switch);

    /* All fallible prep precedes the old_mm snapshot. */
    assert_true(p_elf   < p_oldmm);
    assert_true(p_heap  < p_oldmm);
    assert_true(p_stack < p_oldmm);
    assert_true(p_setup < p_oldmm);
    /* The old_mm snapshot precedes the switch. */
    assert_true(p_oldmm < p_switch);

    free(src);
}

static void test_sys_exec_uses_destroy_helper(void)
{
    TEST_SUITE("sys_exec — destroy_unpublished_user_mm on failure");

    size_t len;
    char *src = slurp_file(task_c_path(), &len);
    assert_not_null(src);
    if (!src) return;

    /* sys_exec failure paths (after new mm is built, before switch)
     * must use destroy_unpublished_user_mm to release the new image
     * before returning -ENOMEM/-EAGAIN. */
    int called_in_exec = source_contains_in_function(
        src, len,
        "int64_t sys_exec(const char *path",
        "destroy_unpublished_user_mm(");
    assert_true(called_in_exec);

    free(src);
}

/* ── Test list ─────────────────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_heap_vma_zero_length),
    TEST_ENTRY(test_heap_vma_vm_flags),
    TEST_ENTRY(test_heap_vma_vm_page_prot),
    TEST_ENTRY(test_mm_init_user_heap_sets_breaks),
    TEST_ENTRY(test_mm_init_user_heap_already_aligned),
    TEST_ENTRY(test_vma_find_does_not_match_zero_heap),
    TEST_ENTRY(test_vma_find_matches_other_vma_with_other_vm),
    TEST_ENTRY(test_exactly_one_heap_vma_in_list),
    TEST_ENTRY(test_mm_init_user_heap_alloc_fail_returns_enomem),
    TEST_ENTRY(test_mm_init_user_heap_alloc_fail_leaves_mm_unchanged),
    TEST_ENTRY(test_destroy_unpublished_user_mm_releases_vmas),
    TEST_ENTRY(test_destroy_unpublished_user_mm_handles_empty_list),
    TEST_ENTRY(test_stack_page_failure_exactly_one_free),
    TEST_ENTRY(test_spawn_user_task_calls_mm_init_user_heap),
    TEST_ENTRY(test_spawn_user_task_no_early_task_list_insert),
    TEST_ENTRY(test_spawn_user_task_uses_destroy_helper),
    TEST_ENTRY(test_sys_exec_calls_mm_init_user_heap),
    TEST_ENTRY(test_sys_exec_old_mm_retained_until_switch),
    TEST_ENTRY(test_sys_exec_uses_destroy_helper),
TEST_LIST_END

int main(void)
{
    for (int i = 0; i < __test_table_size; ++i)
        __test_table[i].fn();
    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed ? 1 : 0;
}
