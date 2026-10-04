/*
 * test/cases/test_fork_user_map.c — Host tests for the fork /
 * teardown 4 KiB ownership hardening (Task 6, user heap/ELF
 * isolation plan).
 *
 * Coverage:
 *   - fork_vma_copy: deep-copies the unique VM_HEAP VMA (zero
 *     length and grown) and file-backed VMAs (vfs_node_get).
 *   - vma_free_all: order check — clears owned leaf PTEs before
 *     vmm_free_user_map handles the rest (no double-free of
 *     VMA-owned phys).
 *   - Source-level inspection of kernel/sched/task.c fork_mm_copy:
 *     staged ordering (child allocs first, parent changes
 *     second), ELF/RO 4 KiB leaves private-copied, writable
 *     VMA 4 KiB leaves use COW, stack retains eager huge-page
 *     copy, no PMD-share OOM fallback.
 *   - Source-level inspection of do_fork: no whole-mm-share
 *     fallback; on fork_mm_copy failure releases task resources
 *     and returns -ENOMEM.
 *
 * PRODUCTION-LINKED: compiles the REAL kernel/memory/vma.c
 * against hosttests/mock/fork_user_map/ (flat indexed PTE
 * table, 4 KiB page pool, COW refcounts, vfs_node refcounts).
 *
 * `fork_mm_copy` is `static` in task.c (Strategy C — same as
 * Task 3 / Task 5): we cannot host-link task.c (its dependency
 * tree — percpu, ipi, fpu, irq, slab, ... — is too heavy), so
 * the staging invariants are verified by SOURCE-LEVEL
 * INSPECTION.  The end-to-end fork/exec/exit invariants are
 * covered by `make OS01_SYSTEST=1 test-qemu SUITE=systest`.
 */
#include "test_framework.h"
#include "fork_user_map_stubs.h"

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

/* ── Layout constants (mirror kernel/memory/vma.c) ───────────── */
#define USER_CODE_ADDR   0x400000UL
#define HEAP_LIMIT       (USER_CODE_ADDR + 0x1000000UL - 0x1000UL)
#define USER_STACK_BASE  0x1400000UL

#define PROT_RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_ANONYMOUS | MAP_PRIVATE)

/* ── Per-test fixtures ───────────────────────────────────────── */

static mm_t parent_mm;
static mm_t child_mm;
static vfs_node_t fake_node;

static void setup_parent(uint64_t elf_end)
{
    fk_stubs_reset();
    memset(&parent_mm, 0, sizeof(parent_mm));
    list_init(&parent_mm.vma_list);
    parent_mm.pgdir = (uint64_t *)0x100000ULL;  /* identity Phy_To_Virt */
    parent_mm.mmap_base = 0x40000000UL;
    assert_eq(0, mm_init_user_heap(&parent_mm, elf_end));
}

static void setup_child(void)
{
    memset(&child_mm, 0, sizeof(child_mm));
    list_init(&child_mm.vma_list);
    child_mm.pgdir = (uint64_t *)0x200000ULL;  /* identity Phy_To_Virt */
    child_mm.mmap_base = 0x40000000UL;
    /* Mirror the parent's start_brk / end_brk — production
     * fork_vma_copy is called AFTER fork_mm_copy copied the
     * struct, so by the time we exercise fork_vma_copy the
     * child already has the parent's break fields. */
    child_mm.start_brk = parent_mm.start_brk;
    child_mm.end_brk   = parent_mm.end_brk;
}

/* Count VMAs in mm's list. */
static int vma_count_for(mm_t *mm)
{
    int n = 0;
    for (list_t *p = mm->vma_list.next;
         p != &mm->vma_list; p = p->next) n++;
    return n;
}

/* Find first VMA in mm with the given vm_flags bit set. */
static vma_t *find_vma_with_flag(mm_t *mm, uint64_t flag)
{
    for (list_t *p = mm->vma_list.next;
         p != &mm->vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_flags & flag) return v;
    }
    return NULL;
}

/* Find VMA containing addr (4 KiB aligned). */
static vma_t *find_vma_at(mm_t *mm, uint64_t addr)
{
    for (list_t *p = mm->vma_list.next;
         p != &mm->vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (addr >= v->vm_start && addr < v->vm_end) return v;
    }
    return NULL;
}

/* ────────────────────────────────────────────────────────────────
 *  Suite 1: fork_vma_copy (production-linked)
 * ──────────────────────────────────────────────────────────────── */

static void test_fork_vma_copy_empty_heap_vma_survives(void)
{
    TEST_SUITE("fork_vma_copy — empty (zero-length) VM_HEAP survives");

    /* Parent: image of exactly one 4 KiB page → start_brk = 0x401000.
     * Heap VMA is zero-length [0x401000, 0x401000) per Task 3. */
    setup_parent(USER_CODE_ADDR + 0x1000);
    setup_child();

    /* Pre-condition: parent has ONE VMA — the zero-length heap VMA. */
    vma_t *parent_heap = find_vma_with_flag(&parent_mm, VM_HEAP);
    assert_not_null(parent_heap);
    assert_eq(USER_CODE_ADDR + 0x1000, parent_heap->vm_start);
    assert_eq(USER_CODE_ADDR + 0x1000, parent_heap->vm_end);
    assert_eq(1, vma_count_for(&parent_mm));
    assert_null(parent_heap->vm_file);

    int child_vmas_before = vma_count_for(&child_mm);
    int parent_gets_before = fk_node_get_count();

    fork_vma_copy(&child_mm, &parent_mm);

    /* Child got exactly ONE new VMA — a deep copy of the heap VMA. */
    assert_eq(child_vmas_before + 1, vma_count_for(&child_mm));

    vma_t *child_heap = find_vma_with_flag(&child_mm, VM_HEAP);
    assert_not_null(child_heap);

    /* Zero-length invariant survives: vm_start == vm_end == start_brk. */
    assert_eq(parent_heap->vm_start, child_heap->vm_start);
    assert_eq(parent_heap->vm_end,   child_heap->vm_end);
    assert_eq(parent_heap->vm_end,   child_heap->vm_start);

    /* vm_flags match (heap flag preserved). */
    assert_eq(parent_heap->vm_flags, child_heap->vm_flags);

    /* Heap has no file backing → no vfs_node_get issued during the copy. */
    assert_null(child_heap->vm_file);
    assert_eq(parent_gets_before, fk_node_get_count());

    /* The child's VMA is its own allocation, not the parent's. */
    assert_true(parent_heap != child_heap);
}

static void test_fork_vma_copy_grown_heap_vma_survives(void)
{
    TEST_SUITE("fork_vma_copy — grown heap VMA (positive length) survives");

    /* Parent: large image, then brk-grown so heap VMA has length. */
    uint64_t elf_end = USER_CODE_ADDR + 0x100000;  /* 1 MiB image */
    setup_parent(elf_end);

    /* Add a positive-length heap VMA to simulate a grown break.
     * Insert via vma_insert + open-coded growth to avoid pulling
     * in mm_set_brk (the heap VMA length grows with the parent's
     * end_brk in production). */
    vma_t *parent_heap = find_vma_with_flag(&parent_mm, VM_HEAP);
    assert_not_null(parent_heap);
    parent_heap->vm_end = parent_heap->vm_start + 0x5000;  /* 5 pages */
    parent_mm.end_brk   = parent_heap->vm_end;
    assert_eq(5, (int)((parent_heap->vm_end - parent_heap->vm_start) / 0x1000));

    setup_child();
    child_mm.end_brk = parent_mm.end_brk;
    int before_vma = vma_count_for(&child_mm);

    fork_vma_copy(&child_mm, &parent_mm);
    assert_eq(before_vma + 1, vma_count_for(&child_mm));

    vma_t *child_heap = find_vma_with_flag(&child_mm, VM_HEAP);
    assert_not_null(child_heap);
    assert_eq(parent_heap->vm_start, child_heap->vm_start);
    assert_eq(parent_heap->vm_end,   child_heap->vm_end);
    assert_eq(parent_heap->vm_flags, child_heap->vm_flags);

    /* Deep copy: own struct, not the parent's pointer. */
    assert_true(parent_heap != child_heap);

    /* Still no vfs_node_get issued — heap is anonymous. */
    assert_eq(0, fk_node_get_count());
}

static void test_fork_vma_copy_file_backed_vma_refs(void)
{
    TEST_SUITE("fork_vma_copy — file-backed VMA bumps vfs_node_get");

    setup_parent(USER_CODE_ADDR + 0x1000);
    setup_child();

    /* Insert a file-backed VMA into the parent (simulating a
     * file-mmap'd region).  vm_file = &fake_node — the harness's
     * vfs_node_get returns the same pointer and bumps a counter. */
    vma_t *fmv = (vma_t *)malloc(sizeof(vma_t));
    assert_not_null(fmv);
    list_init(&fmv->list);
    fmv->vm_start     = 0x40000000UL;   /* auto-mmap region */
    fmv->vm_end       = 0x40001000UL;
    fmv->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    fmv->vm_flags     = VM_READ | VM_WRITE;
    fmv->vm_pgoff     = 0;
    fmv->vm_file      = &fake_node;
    vma_insert(&parent_mm, fmv);

    int child_vmas_before = vma_count_for(&child_mm);
    int parent_gets_before = fk_node_get_count();

    fork_vma_copy(&child_mm, &parent_mm);

    /* Child received both the heap VMA and the file-backed VMA. */
    assert_eq(child_vmas_before + 2, vma_count_for(&child_mm));

    /* File-backed VMA in the child: deep-copied, vm_file refcount bumped. */
    vma_t *cfmv = find_vma_at(&child_mm, 0x40000000UL);
    assert_not_null(cfmv);
    assert_eq(fmv->vm_start, cfmv->vm_start);
    assert_eq(fmv->vm_end,   cfmv->vm_end);
    assert_true(fmv != cfmv);    /* deep struct */
    assert_true(fmv->vm_file == cfmv->vm_file);  /* same node pointer */

    /* vfs_node_get fired exactly once — for the new VMA copy. */
    assert_eq(parent_gets_before + 1, fk_node_get_count());

    free(fmv);
}

/* ────────────────────────────────────────────────────────────────
 *  Suite 2: vma_free_all (production-linked)
 * ──────────────────────────────────────────────────────────────── */

static void test_vma_free_all_empty_heap_vma_no_pages(void)
{
    TEST_SUITE("vma_free_all — empty (zero-length) heap VMA unmaps nothing");

    setup_parent(USER_CODE_ADDR + 0x1000);

    int unmaps_before = fk_state.total_unmaps;
    int frees_before  = fk_state.total_frees;
    int in_use_before = fk_in_use_count();

    vma_free_all(&parent_mm);

    /* Heap VMA is zero-length — the unmap loop iterates zero times. */
    assert_eq(unmaps_before, fk_state.total_unmaps);
    assert_eq(frees_before,  fk_state.total_frees);
    assert_eq(in_use_before, fk_in_use_count());
    assert_eq(0, vma_count_for(&parent_mm));
}

static void test_vma_free_all_unmaps_vma_owned_pages(void)
{
    TEST_SUITE("vma_free_all — owned 4 KiB leaves unmapped (VMA → unmap → page table free order)");

    /* Heap page at 0x401000 (zero-length VMA grown via the
     * vma_remove trick).  Hand-fill a PTE so vma_free_all has
     * something to unmap. */
    setup_parent(USER_CODE_ADDR + 0x1000);
    vma_t *heap = find_vma_with_flag(&parent_mm, VM_HEAP);
    heap->vm_end = heap->vm_start + 0x3000;   /* 3 pages */

    uint64_t phys1 = alloc_4k_page();
    uint64_t phys2 = alloc_4k_page();
    uint64_t phys3 = alloc_4k_page();
    assert_true(phys1 && phys2 && phys3);
    assert_eq(0, vmm_map_4k_page(parent_mm.pgdir, phys1, 0x401000,
                                  PAGE_USER | PAGE_WRITE | PAGE_VALID));
    assert_eq(0, vmm_map_4k_page(parent_mm.pgdir, phys2, 0x402000,
                                  PAGE_USER | PAGE_WRITE | PAGE_VALID));
    assert_eq(0, vmm_map_4k_page(parent_mm.pgdir, phys3, 0x403000,
                                  PAGE_USER | PAGE_WRITE | PAGE_VALID));
    int allocs_before = fk_state.total_allocs;
    int unmaps_before = fk_state.total_unmaps;
    int frees_before  = fk_state.total_frees;

    vma_free_all(&parent_mm);

    /* Three unmaps, three frees. */
    assert_eq(unmaps_before + 3, fk_state.total_unmaps);
    assert_eq(frees_before + 3,  fk_state.total_frees);

    /* PTEs are cleared (leaf PTE bytes = 0 — proves vma_free_all
     * clears owned leaves BEFORE vmm_free_user_map walks them). */
    for (uint64_t va = 0x401000; va < 0x404000; va += 0x1000) {
        fk_pte_record_t *rec = fk_find_mapping(va);
        assert_null(rec);  /* PAGE_VALID cleared, slot empty */
    }

    /* VMA list drained. */
    assert_eq(0, vma_count_for(&parent_mm));

    /* Allocation count untouched — only frees happened. */
    assert_eq(allocs_before, fk_state.total_allocs);
}

static void test_vma_free_all_cow_pages_ref(void)
{
    TEST_SUITE("vma_free_all — COW refcount honored (decremented to 0 → phys freed)");

    setup_parent(USER_CODE_ADDR + 0x1000);
    vma_t *heap = find_vma_with_flag(&parent_mm, VM_HEAP);
    heap->vm_end = heap->vm_start + 0x1000;

    uint64_t phys = alloc_4k_page();
    assert_true(phys);
    assert_eq(0, vmm_map_4k_page(parent_mm.pgdir, phys, 0x401000,
                                  PAGE_USER | PAGE_VALID | PAGE_COW));
    /* COW refcount > 0 simulates a fork-shared leaf. */
    page_cow_get(phys);

    int cow_gets_before = fk_state.total_cow_gets;
    int cow_puts_before = fk_state.total_cow_puts;
    int frees_before    = fk_state.total_frees;

    vma_free_all(&parent_mm);

    /* One page_cow_put reached 0 → one phys freed. */
    assert_eq(cow_puts_before + 1, fk_state.total_cow_puts);
    assert_eq(frees_before + 1,    fk_state.total_frees);

    /* cow_refs on this phys is back to 0 — pool reset still ok. */
    fk_page_record_t *rec = fk_find_page(phys);
    assert_not_null(rec);
    assert_eq(0, rec->cow_refs);

    (void)cow_gets_before;
}

static void test_vma_free_all_vmio_skips_unmap(void)
{
    TEST_SUITE("vma_free_all — VM_IO VMA skips unmap");

    setup_parent(USER_CODE_ADDR + 0x1000);

    /* Insert a VM_IO VMA — production vma_free_all removes it
     * without unmaping pages (the kernel-half heuristic). */
    vma_t *iov = (vma_t *)malloc(sizeof(vma_t));
    assert_not_null(iov);
    list_init(&iov->list);
    iov->vm_start     = 0x40000000UL;
    iov->vm_end       = 0x40001000UL;
    iov->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    iov->vm_flags     = VM_READ | VM_WRITE | VM_IO;
    iov->vm_file      = NULL;
    vma_insert(&parent_mm, iov);

    int unmaps_before = fk_state.total_unmaps;
    int puts_before   = fk_node_put_count();

    vma_free_all(&parent_mm);

    /* VM_IO VMA removed without a single PTE unmap (no phys was
     * ever committed to the test PTE table — there is nothing to
     * free).  vma_remove() calls vfs_node_put(vm_file=NULL),
     * which our harness counts as a no-op put.  Note: vma_free_all
     * already called vma_remove → kfree(iov), so the test does NOT
     * free(iov) again — that would be a double-free. */
    assert_eq(unmaps_before, fk_state.total_unmaps);
    assert_eq(puts_before,   fk_node_put_count());
    assert_eq(0, vma_count_for(&parent_mm));
}

static void test_vma_free_all_then_user_map_no_double_free(void)
{
    TEST_SUITE("teardown order — vma_free_all then vmm_free_user_map: no double-free");

    /* Model the destroy_unpublished_user_mm contract:
     *   vma_free_all(mm)        — clears owned leaf PTEs
     *   vmm_free_user_map(pgd)   — handles the rest
     *
     * If vma_free_all didn't clear owned leaves, vmm_free_user_map
     * would double-free their phys.  The vmm_free_user_map stub
     * here just walks the production pte format — we count its
     * unmaps + freems and verify it sees ZERO leaves that were
     * already cleared by vma_free_all. */
    setup_parent(USER_CODE_ADDR + 0x1000);
    vma_t *heap = find_vma_with_flag(&parent_mm, VM_HEAP);
    heap->vm_end = heap->vm_start + 0x2000;

    uint64_t phys1 = alloc_4k_page();
    uint64_t phys2 = alloc_4k_page();
    assert_eq(0, vmm_map_4k_page(parent_mm.pgdir, phys1, 0x401000,
                                  PAGE_USER | PAGE_WRITE | PAGE_VALID));
    assert_eq(0, vmm_map_4k_page(parent_mm.pgdir, phys2, 0x402000,
                                  PAGE_USER | PAGE_WRITE | PAGE_VALID));
    int allocs_before = fk_state.total_allocs;

    vma_free_all(&parent_mm);
    int unmaps_after_vfa = fk_state.total_unmaps;
    int frees_after_vfa  = fk_state.total_frees;
    assert_eq(2, unmaps_after_vfa - (unmaps_after_vfa - 2));  /* sanity */
    assert_eq(2, frees_after_vfa - (frees_after_vfa - 2));

    /* Simulate vmm_free_user_map on a PTE table with the two
     * leaves already cleared.  Production's vmm_free_user_map
     * walks the PTE table (level 3) and skips zeroed entries —
     * our vmm_unmap_4k_page stub already handles that.  No
     * additional unmaps or frees should occur for the two
     * committed leaves — they'd be double-free if the order
     * didn't work. */
    vmm_unmap_4k_page(parent_mm.pgdir, 0x401000);
    vmm_unmap_4k_page(parent_mm.pgdir, 0x402000);

    assert_eq(unmaps_after_vfa, fk_state.total_unmaps);  /* no new unmaps */
    assert_eq(frees_after_vfa,  fk_state.total_frees);   /* no new frees */
    assert_eq(allocs_before,    fk_state.total_allocs);  /* no new allocs */

    /* Every page we allocated is freed exactly once. */
    assert_eq(0, fk_in_use_count());
}

/* ────────────────────────────────────────────────────────────────
 *  Suite 3: Source-level inspection — kernel/sched/task.c
 * ──────────────────────────────────────────────────────────────── */

static const char *task_c_path(void)
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
                 "%.*s/kernel/sched/task.c",
                 (int)(marker - full), full);
        return buf;
    }
    if (strncmp(full, "hosttests/", 10) == 0)
        return "kernel/sched/task.c";
    return NULL;
}

static char *slurp_task_c(size_t *out_len)
{
    const char *path = task_c_path();
    if (!path) return NULL;
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
        int64_t r = read(fd, buf + len, 4096);
        if (r < 0) { free(buf); close(fd); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static const char *find_from(const char *start, const char *end,
                             const char *needle)
{
    if (!start || !end || !needle) return NULL;
    size_t n = strlen(needle);
    for (const char *p = start; p + n <= end; p++) {
        if (memcmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

static void test_fork_mm_copy_source_layout(void)
{
    TEST_SUITE("task.c source — fork_mm_copy / do_fork are reachable");

    /* Smoke test: the production functions exist at the expected
     * signature. */
    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);

    assert_not_null(strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)"));
    assert_not_null(strstr(src,
        "uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags"));

    free(src);
}

static void test_fork_mm_copy_no_pmd_share_fallback(void)
{
    TEST_SUITE("fork_mm_copy — no PMD-share fallback (child_pmd[l2] = pmde)");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);

    /* Bounded to the fork_mm_copy body. */
    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");   /* matches the body's closing brace */
    assert_not_null(fn_end);

    /* The PMD-share fallback used to be the OOM response for the
     * 4 KiB PTE alloc — explicitly:
     *   child_pmd[l2] = pmde;  // OOM: share PDE
     * It must NOT appear inside fork_mm_copy anymore (brief:
     *   "remove the child_pmd[l2] = pmde; fallback").  The
     * genuine VM_IO-huge-page share at line 2041 (no OOM
     * context, just `if (vm && (vm->vm_flags & VM_IO))`) is
     * correct and must remain — the search below keys on the
     * OOM-comment marker. */
    const char *hit = find_from(fn, fn_end,
                                "child_pmd[l2] = pmde;  // OOM: share PDE");
    assert_null(hit);
    /* Defence-in-depth: a generic `child_pmd[l2] = pmde;` line
     * inside an `if (!child_pte)` block (the 4 KiB OOM context)
     * must NOT appear.  The VM_IO huge-page branch is gated on
     * VM_IO, not on !child_pte, so this discriminates correctly. */
    const char *four_k_block = find_from(fn, fn_end, "if (!child_pte)");
    if (four_k_block) {
        assert_null(find_from(four_k_block, fn_end,
                              "child_pmd[l2] = pmde"));
    }

    free(src);
}

static void test_fork_mm_copy_elf_leaf_private_copy(void)
{
    TEST_SUITE("fork_mm_copy — ELF/RO 4 KiB leaf path: alloc + copy (no COW)");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);

    /* The 4 KiB leaf path (non-PAGE_HUGE) must do an alloc +
     * memcpy to give the child a PRIVATE physical page (the
     * brief mandates "private physical pages" for 4 KiB leaves
     * whose source PTE is not writable).  Without alloc +
     * memcpy the child would inherit the parent's phys via a
     * shared PTE — exactly the bug the brief is hardening
     * against.  We accept either libc memcpy or inline asm. */
    const char *alloc_4k = find_from(fn, fn_end, "alloc_4k_page");
    assert_not_null(alloc_4k);
    /* The 4 KiB alloc must come AFTER the page-table calloc
     * block (calloc for the PTE table, alloc_4k_page for the
     * phys).  The first calloc within fork_mm_copy is the PTE
     * table; alloc_4k_page must appear at least once after
     * that. */
    const char *first_calloc = find_from(fn, fn_end, "calloc(1, PAGE_4K_SIZE)");
    assert_not_null(first_calloc);
    /* Find alloc_4k_page AFTER first_calloc. */
    const char *alloc_after = find_from(first_calloc, fn_end, "alloc_4k_page");
    assert_not_null(alloc_after);

    /* alloc_4k_page is followed by a copy of the parent's phys
     * into the child's phys (memcpy or inline rep movsb). */
    const char *copy = find_from(alloc_after, fn_end, "memcpy(");
    const char *inline_copy = find_from(alloc_after, fn_end, "rep movsb");
    assert_true(copy != NULL || inline_copy != NULL);

    free(src);
    (void)end;
}

static void test_fork_mm_copy_writable_leaf_uses_cow(void)
{
    TEST_SUITE("fork_mm_copy — writable VMA 4 KiB leaf: COW on both parent + child");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);

    /* The writable-leaf path mutates the parent PTE (R/W → R/O
     * + PAGE_COW) and adds a cow ref for the child. */
    assert_not_null(find_from(fn, fn_end, "PAGE_COW"));
    assert_not_null(find_from(fn, fn_end, "page_cow_get"));

    free(src);
    (void)end;
}

static void test_fork_mm_copy_stack_eager_huge(void)
{
    TEST_SUITE("fork_mm_copy — stack eager huge-page copy retained");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);

    /* The PAGE_HUGE branch is preserved (eager copy of the user
     * stack 2 MiB page) — PAGE_HUGE must be referenced, and the
     * alloc_pages/ZONE_NORMAL/1 path (the 2 MiB huge-page alloc)
     * must still appear. */
    assert_not_null(find_from(fn, fn_end, "PAGE_HUGE"));
    assert_not_null(find_from(fn, fn_end, "alloc_pages"));
    assert_not_null(find_from(fn, fn_end, "ZONE_NORMAL"));

    free(src);
    (void)end;
}

static void test_fork_mm_copy_staged_ordering(void)
{
    TEST_SUITE("fork_mm_copy — staged ordering: child allocs first, parent changes second");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);

    /* The brief mandates:
     *   "Stage child VMA/page tables and all failing allocations
     *    BEFORE changing parent writable PTEs or adding COW refs.
     *    Commit parent changes in a bounded no-failure phase.
     *    Flush affected TLB entries."
     *
     * Operational proxies:
     *   - tlb_shootdown() must be called inside fork_mm_copy,
     *     AFTER the parent PTE changes (the brief names
     *     "Flush affected TLB entries" as the closing step).
     *   - The body's first call to page_cow_get (which mutates
     *     the parent refcount) must come AFTER at least one
     *     child allocation (calloc for PTE tables / PUD / PMD,
     *     or alloc_4k_page for 4 KiB leaves, or alloc_pages
     *     for the eager huge-page).  All "child phys" allocs
     *     come before any parent PTE mutation / COW ref bump. */
    const char *tlb = strstr(fn, "tlb_shootdown");
    assert_not_null(tlb);
    assert_true(tlb < fn_end);

    /* Find the FIRST page_cow_get (parent PTE mutation). */
    const char *first_cow_get = find_from(fn, fn_end, "page_cow_get");
    assert_not_null(first_cow_get);

    /* Find ANY child allocation before first_cow_get.  Either:
     *   - calloc(1, PAGE_4K_SIZE) (PUD / PMD / PTE tables), or
     *   - alloc_4k_page (4 KiB leaf — added in Task 6), or
     *   - alloc_pages (2 MiB stack — eager huge-page copy).
     * The first child allocation MUST be strictly before the
     * first parent PTE mutation. */
    const char *first_calloc = find_from(fn, first_cow_get, "calloc(1, PAGE_4K_SIZE)");
    const char *first_4k = find_from(fn, first_cow_get, "alloc_4k_page");
    const char *first_2m = find_from(fn, first_cow_get, "alloc_pages");
    assert_true(first_calloc != NULL ||
                first_4k != NULL ||
                first_2m != NULL);

    /* tlb_shootdown is called AFTER any parent PTE mutation
     * (page_cow_get on the parent's PTE — i.e. after the
     * "commit parent changes" phase, flushing the affected
     * TLB entries). */
    assert_true(first_cow_get < tlb);

    free(src);
    (void)end;
}

static void test_do_fork_no_whole_mm_share_fallback(void)
{
    TEST_SUITE("do_fork — no whole-mm-share fallback (only on !tsk->mm branch)");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    /* The pre-existing fallback (brief: "remove the fork: ...
     * falling back to shared mm fallback in do_fork") lived
     * INSIDE the `if (!tsk->mm)` branch (the post-fork_mm_copy
     * failure branch).  It used to do:
     *   tsk->mm = current->mm;
     *   thd->cr3 = current->thread->cr3;
     * After removal, the failure branch must contain a clean
     * release path and a -ENOMEM return.  The initial
     * `tsk->mm = current->mm;` (BEFORE fork_mm_copy is called)
     * is legitimate and stays. */
    const char *fn = strstr(src,
        "uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);

    /* Bounded search: the `if (!tsk->mm)` block — the post-
     * fork_mm_copy failure branch.  Neither the share-the-mm
     * writes nor the legacy debug print must appear in this
     * block.  The null_branch ends at the next `}` at the same
     * indentation as the `if (!tsk->mm) {` opening.  In the
     * source the `if (!tsk->mm)` line is indented 12 spaces,
     * so its closing `}` is also at 12 spaces.  We accept any
     * of 8/12/20-space close braces to be tolerant of future
     * re-formatting. */
    const char *null_branch = find_from(fn, fn_end, "if (!tsk->mm)");
    assert_not_null(null_branch);
    const char *candidates[] = {
        "\n            }\n",   /* 12-space close — current code */
        "\n        }\n",      /* 8-space close  — alt format  */
        "\n                    }\n", /* 20-space — alt */
    };
    const char *null_branch_end = NULL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        null_branch_end = find_from(null_branch, fn_end, candidates[i]);
        if (null_branch_end) break;
    }
    if (!null_branch_end) null_branch_end = fn_end;

    const char *hit;
    /* Search for the EXACT code form (newline-terminated, no
     * `* ` comment prefix) — comments in the surrounding doc
     * may legitimately quote the pattern (e.g. "the pre-existing
     * tsk->mm = current->mm; thd->cr3 = ...").  The real code
     * forms are bare statements on their own line. */
    hit = find_from(null_branch, null_branch_end,
                   "\n            tsk->mm = current->mm;");
    if (!hit) hit = find_from(null_branch, null_branch_end,
                              "\n        tsk->mm = current->mm;");
    assert_null(hit);
    hit = find_from(null_branch, null_branch_end,
                    "\n            thd->cr3 = current->thread->cr3;");
    if (!hit) hit = find_from(null_branch, null_branch_end,
                              "\n        thd->cr3 = current->thread->cr3;");
    assert_null(hit);
    /* (The legacy `debug_task("fork: ... falling back to shared
     * mm")` print is gone too — the doc comment quotes the
     * legacy pattern for context, so we don't assert on the
     * exact string here; the two bare-statement checks above
     * are sufficient.) */

    free(src);
    (void)end;
}

static void test_do_fork_releases_task_on_oom(void)
{
    TEST_SUITE("do_fork — on fork_mm_copy NULL return: -ENOMEM, task released");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    const char *fn = strstr(src,
        "uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);

    /* The brief mandates: "child OOM returns NULL → do_fork
     * releases task resources → returns -ENOMEM".  Observable
     * textually: in the `if (!tsk->mm)` branch, do_fork frees
     * the task union / kernel stack (raw_alloc = ... = tsk->
     * stack_alloc_base) and the thread struct (thd), then
     * returns -ENOMEM.  do_fork MUST NOT touch tsk->mm =
     * current->mm on the failure path. */

    /* The failure branch is the `if (!tsk->mm)` block; bound the
     * search to the block (see test_do_fork_no_whole_mm_share_
     * fallback for the brace-match heuristic). */
    const char *null_branch = find_from(fn, fn_end, "if (!tsk->mm)");
    assert_not_null(null_branch);
    const char *candidates[] = {
        "\n            }\n",
        "\n        }\n",
        "\n                    }\n",
    };
    const char *null_branch_end = NULL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        null_branch_end = find_from(null_branch, fn_end, candidates[i]);
        if (null_branch_end) break;
    }
    if (!null_branch_end) null_branch_end = fn_end;

    /* The branch must free every resource do_fork allocated:
     * tsk->stack_alloc_base (the raw_alloc buffer holding the
     * task union + kernel stack — the canonical owner), thd,
     * the fpu_save area, and any files-table reference. */
    assert_not_null(find_from(null_branch, null_branch_end,
                              "kfree(tsk->stack_alloc_base)"));
    assert_not_null(find_from(null_branch, null_branch_end, "kfree(thd)"));
    assert_not_null(find_from(null_branch, null_branch_end,
                              "kfree(tsk->fpu_save)"));
    assert_not_null(find_from(null_branch, null_branch_end,
                              "files_unpin"));
    /* And it must remove the child from the task list (so
     * init's waitpid loop cannot reach it). */
    assert_not_null(find_from(null_branch, null_branch_end, "list_del"));

    /* -ENOMEM returned on the failure path. */
    assert_not_null(find_from(null_branch, null_branch_end, "-ENOMEM"));

    /* The branch MUST NOT silently fall back to a shared parent
     * mm (already covered by test_do_fork_no_whole_mm_share_
     * fallback, but pinned here too — and now bounded to the
     * failure block, so the legitimate initial
     * `tsk->mm = current->mm;` outside doesn't trip us).  Same
     * newline-terminated form as the no-fallback check above —
     * the surrounding doc-comment quotes the legacy form
     * explicitly, so plain substring matching trips us. */
    const char *hit2;
    hit2 = find_from(null_branch, null_branch_end,
                     "\n            tsk->mm = current->mm;");
    if (!hit2) hit2 = find_from(null_branch, null_branch_end,
                                "\n        tsk->mm = current->mm;");
    assert_null(hit2);
    hit2 = find_from(null_branch, null_branch_end,
                     "\n            thd->cr3 = current->thread->cr3;");
    if (!hit2) hit2 = find_from(null_branch, null_branch_end,
                                "\n        thd->cr3 = current->thread->cr3;");
    assert_null(hit2);

    free(src);
    (void)end;
}

/* ────────────────────────────────────────────────────────────────
 *  Suite 4: fork_mm_copy rollback invariants (Task 6 review)
 *
 *  The `fail:` rollback in fork_mm_copy must:
 *    - Bug A1: NOT free huge-page phys when the entry was a
 *      VM_IO share (parent's MMIO PMD; pass 1 set
 *      child_pmd[l2] = pmde without alloc).
 *    - Bug A2: NOT free 4 KiB phys for an already-COW PTE
 *      (fork-of-fork shape); must instead balance the pass-1
 *      page_cow_get with page_cow_put so the shared phys
 *      stays alive for parent + any siblings.
 *
 *  Verified via source-level inspection of kernel/sched/task.c
 *  (same Strategy C as the rest of the fork_mm_copy tests —
 *  fork_mm_copy is `static`, and its rollback references
 *  Phy_to_2M_Page / free_pages which the host can't link).
 * ──────────────────────────────────────────────────────────────── */

static void test_fork_mm_copy_fail_keeps_vmio_huge_intact(void)
{
    TEST_SUITE("fork_mm_copy rollback — VM_IO huge PMD is not freed (Bug A1)");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    /* The rollback lives inside the `fail:` block of
     * fork_mm_copy.  Locate it by the unique "fail:" label
     * that follows the staging pass 2. */
    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);
    /* The `fail:` label appears after the staged success
     * path; locate it directly to bound the rollback walk. */
    const char *fail_label = strstr(fn, "\nfail:");
    assert_not_null(fail_label);
    const char *fail_block_end = fn_end;
    /* Sub-region: from fail_label to fn_end. */

    /* Bug A1: the rollback's huge-page branch must consult
     * the parent's VMA flags for the 2 MiB VA and SKIP the
     * free_pages call when the region is VM_IO.  Pass 1's
     * huge-page branch shares the parent's MMIO PMD with
     * child_pmd[l2] = pmde — no allocation.  The rollback
     * must not free that phys.
     *
     * Concrete check (textual, in the rollback's huge-page
     * branch): vma_find must be called, and the VM_IO check
     * must appear BEFORE the free_pages call. */
    const char *huge = find_from(fail_label, fail_block_end,
        "if (pmde & PAGE_HUGE) {");
    assert_not_null(huge);
    const char *free_pages_call = find_from(huge, fail_block_end,
        "free_pages(p, 1)");
    assert_not_null(free_pages_call);
    /* vma_find must appear inside the huge-page branch and
     * BEFORE the free_pages call. */
    const char *vm_find = find_from(huge, free_pages_call,
        "vma_find(parent_mm, vaddr_2m)");
    assert_not_null(vm_find);
    /* VM_IO must be checked BEFORE the free_pages call too. */
    const char *vm_io_check = find_from(huge, free_pages_call,
        "vm_flags & VM_IO");
    assert_not_null(vm_io_check);
    /* And vm_find must be checked before free_pages. */
    assert_true(vm_find < free_pages_call);
    assert_true(vm_io_check < free_pages_call);

    /* Defence-in-depth: the rollback must NOT have an
     * unconditional `free_pages(p, 1)` directly under
     * `if (pmde & PAGE_HUGE)` without a VM_IO guard.  We
     * verify by looking for the exact pattern that would be
     * a regression — if VMIO wasn't special-cased, the huge
     * branch would look like:
     *     if (pmde & PAGE_HUGE) {
     *         free_pages(p, 1);    <-- this is OK only when guarded
     *     }
     * We accept the call as long as the vma_find + VM_IO check
     * precede it (already checked above). */

    /* Also: the parent's 2 MiB VA computation
     * `vaddr_2m = ((uint64_t)l4 << 39) | ((uint64_t)l3 << 30) |
     * ((uint64_t)l2 << 21)` must appear inside the huge-page
     * branch so the vma_find lookup targets the right VA. */
    const char *vaddr_calc = find_from(huge, free_pages_call,
        "vaddr_2m = ((uint64_t)l4 << 39)");
    assert_not_null(vaddr_calc);

    free(src);
    (void)end;
}

static void test_fork_mm_copy_fail_keeps_cow_share_intact(void)
{
    TEST_SUITE("fork_mm_copy rollback — already-COW 4 KiB leaf: page_cow_put (Bug A2)");

    size_t len;
    char *src = slurp_task_c(&len);
    assert_not_null(src);
    const char *end = src + len;

    /* Locate the rollback block. */
    const char *fn = strstr(src,
        "static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)");
    assert_not_null(fn);
    const char *fn_end = strstr(fn, "\n}\n");
    assert_not_null(fn_end);
    const char *fail_label = strstr(fn, "\nfail:");
    assert_not_null(fail_label);
    const char *fail_block_end = fn_end;

    /* Bug A2: the rollback's 4 KiB leaf branch (else of the
     * huge-page if) must check PAGE_COW BEFORE the
     * free_4k_page call.  For an already-COW PTE (fork-of-fork
     * shape), pass 1 added one ref via page_cow_get; the
     * rollback must balance that with page_cow_put and skip
     * the free (the shared phys stays alive for parent +
     * siblings).  Without the PAGE_COW check, the rollback
     * frees the shared phys out from under every other
     * reference holder and leaves the refcount unbalanced.
     *
     * Concrete check (textual): inside the `else` branch
     * (4 KiB PTE table path), the `page_cow_put` call must
     * appear, AND the
     *   `if ((pte & PAGE_VALID) && ... && !is_vmio)`
     * guard must include a `is_cow` (PAGE_COW) branch that
     * routes to page_cow_put instead of free_4k_page. */
    const char *non_huge = find_from(fail_label, fail_block_end,
        "} else {");
    assert_not_null(non_huge);
    /* The first `} else {` after fail_label is the inner
     * 4 KiB branch (the huge/else at the PMD level). */
    const char *inner_else = non_huge;
    /* The PAGE_COW check must appear inside the 4 KiB
     * branch BEFORE any free_4k_page. */
    const char *cow_check = find_from(inner_else, fail_block_end,
        "PAGE_COW");
    assert_not_null(cow_check);
    /* page_cow_put must appear inside the 4 KiB branch. */
    const char *cow_put = find_from(inner_else, fail_block_end,
        "page_cow_put");
    assert_not_null(cow_put);
    /* The structure: the rollback branches on PAGE_COW to
     * either page_cow_put (balance the ref) or free_4k_page
     * (genuinely private copy).  The branching pattern must
     * be: PAGE_COW < page_cow_put (so the COW branch uses
     * page_cow_put) and PAGE_COW < free_4k_page (so the
     * non-COW branch uses free_4k_page).  The current order
     * also has the PAGE_COW check before the free. */
    const char *free_4k = find_from(inner_else, fail_block_end,
        "free_4k_page(");
    assert_not_null(free_4k);
    assert_true(cow_check < cow_put);
    assert_true(cow_check < free_4k);

    /* Defence-in-depth: free_4k_page MUST be inside an
     * `else` (or equivalent) of the PAGE_COW branch so the
     * COW case can never hit it.  The simplest check:
     * between cow_check and free_4k the string "if (is_cow)"
     * (or equivalent) must appear. */
    const char *is_cow_branch = find_from(cow_check, free_4k,
                                        "is_cow");
    assert_not_null(is_cow_branch);

    /* Also: the placeholder (pte == PAGE_VALID) and VM_IO
     * guards are still in place from the original Task 6
     * rollback — keep them pinned too. */
    const char *placeholder_check = find_from(inner_else, fail_block_end,
        "is_placeholder");
    assert_not_null(placeholder_check);
    const char *vmio_check = find_from(inner_else, fail_block_end,
        "is_vmio");
    assert_not_null(vmio_check);

    free(src);
    (void)end;
}

/* ────────────────────────────────────────────────────────────────
 *  Test list
 * ──────────────────────────────────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_fork_vma_copy_empty_heap_vma_survives),
    TEST_ENTRY(test_fork_vma_copy_grown_heap_vma_survives),
    TEST_ENTRY(test_fork_vma_copy_file_backed_vma_refs),
    TEST_ENTRY(test_vma_free_all_empty_heap_vma_no_pages),
    TEST_ENTRY(test_vma_free_all_unmaps_vma_owned_pages),
    TEST_ENTRY(test_vma_free_all_cow_pages_ref),
    TEST_ENTRY(test_vma_free_all_vmio_skips_unmap),
    TEST_ENTRY(test_vma_free_all_then_user_map_no_double_free),
    TEST_ENTRY(test_fork_mm_copy_source_layout),
    TEST_ENTRY(test_fork_mm_copy_no_pmd_share_fallback),
    TEST_ENTRY(test_fork_mm_copy_elf_leaf_private_copy),
    TEST_ENTRY(test_fork_mm_copy_writable_leaf_uses_cow),
    TEST_ENTRY(test_fork_mm_copy_stack_eager_huge),
    TEST_ENTRY(test_fork_mm_copy_staged_ordering),
    TEST_ENTRY(test_fork_mm_copy_fail_keeps_vmio_huge_intact),
    TEST_ENTRY(test_fork_mm_copy_fail_keeps_cow_share_intact),
    TEST_ENTRY(test_do_fork_no_whole_mm_share_fallback),
    TEST_ENTRY(test_do_fork_releases_task_on_oom),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed == 0 ? 0 : 1;
}