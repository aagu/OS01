// kernel/selftest/test_deep_copy_argv.c — regression test for
// kernel/arch/x86_64/trap.c::deep_copy_argv (Task 4.5, startup
// unification).
//
// Bug under test (pre-fix): deep_copy_argv() rejected an explicit
// empty pointer array (user_arr = {NULL}) with -E2BIG.  POSIX requires
// argv=NULL AND argv={NULL} to be accepted by execve (kernel/sched/
// task.c::setup_user_stack already handles both).  The new libc
// __libc_start_main stores `environ` as a non-NULL pointer to {NULL}
// (the empty-env terminator), so exec() of any program with envc==0
// started going through deep_copy_argv → -E2BIG → exec failure.
// The same issue blocked the systest startup-probe matrix's argv0
// cases (argc=0 / envc=0/1) and the boundary argv0/envc=128 probe.
//
// This test:
//   1. Splits the boot kernel's 2MB kernel-only PDE at 0x600000 and
//      installs a 4KB user PTE pointing at a fresh 2MB block from the
//      page allocator (mirrors test_uaccess.c step 4).
//   2. Writes {NULL} (8 zero bytes) to the user page via copy_to_user_ft
//      under a temporarily user-mode addr_limit.
//   3. Calls deep_copy_argv() directly with a pointer to that array.
//   4. Asserts rc == 0 (NOT -E2BIG) and *out_arr != NULL with the
//      terminator at [0] == NULL (the same shape setup_user_stack
//      would walk via the s_argc==0 path).
//   5. Restores addr_limit, frees the kernel-side argv copy, restores
//      the page table, and frees the 2MB block.
//
// Registered in selftest_run_all() under the OS01_SELFTEST gate.
//
// Pre-fix:  rc == -E2BIG  → FAIL
// Post-fix: rc == 0, kargv != NULL, kargv[0] == NULL  → PASS

#if defined(OS01_SELFTEST)

#include <core/selftest.h>
#include <core/printk.h>
#include <memory/uaccess.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <memory/slab.h>   // kfree
#include <sched/task.h>
#include <arch/mmu.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// ── Assertion helpers (mirror test_uaccess.c style) ─────────
#define SELFTEST_ASSERT(cond)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            serial_printk("[selftest] deep_copy_argv: FAIL @ %s:%d: %s\n", \
                __FILE__, __LINE__, #cond);                                 \
            return -1;                                                      \
        }                                                                   \
    } while (0)

#define SELFTEST_FAIL_AT(fmt, ...)                                         \
    do {                                                                    \
        serial_printk("[selftest] deep_copy_argv: FAIL @ %s:%d: " fmt "\n", \
            __FILE__, __LINE__, ##__VA_ARGS__);                             \
        return -1;                                                          \
    } while (0)

// ── deep_copy_argv under test ──────────────────────────────
// Static in trap.c; exposed via OS01_SELFTEST for this test only.
int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr);

// Free a deep_copy_argv result (kernel-side array+strings).
// Mirrors trap.c::free_deep_argv — duplicated here so the test doesn't
// depend on its static linkage.
static void dca_free(char **kargv)
{
    if (!kargv) return;
    for (size_t i = 0; kargv[i] != NULL; i++) kfree(kargv[i]);
    kfree(kargv);
}

// ── Self-contained page-table setup (mirrors test_uaccess.c) ──
//
// ensure_pt: returns a pointer to the leaf PTE slot for `va`.  Splits
// a 2MB huge PDE into 512 4KB PTEs if needed, preserving the original
// mapping for the other 511 pages.  Records every modification in `ctx`
// so dca_restore_pt can roll back exactly and free the new tables.
//
// All four levels use the live kernel CR3 (selftest runs from init_task
// — see test_uaccess.c step 4).
struct dca_map_ctx {
    uint64_t  va;
    uint64_t  saved_l4, saved_l3, saved_l2;
    uint64_t  saved_pmd;        // original 2MB PMD if we split
    uint64_t  *leaf_slot;       // pointer into a table to leaf PTE
    // Bookkeeping for freeing test-only page-table allocations.
    struct Page *new_l3_page, *new_l2_page, *new_pte_page;
    int       created_l3, created_l2, created_pte, split_2m;
};

static struct Page *dca_alloc_pgtbl_zeroed(void)
{
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) return (struct Page *)0;
    uint64_t *v = (uint64_t *)Phy_To_Virt(pg->phy_address);
    memset(v, 0, PAGE_4K_SIZE);
    return pg;
}

static uint64_t *dca_ensure_pt(uint64_t *pgd, uint64_t va, struct dca_map_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->va = va;
    uint64_t l4 = (va >> 39) & 0x1FF;
    uint64_t l3 = (va >> 30) & 0x1FF;
    uint64_t l2 = (va >> 21) & 0x1FF;
    uint64_t l1 = (va >> 12) & 0x1FF;

    ctx->saved_l4 = pgd[l4];
    if (!(pgd[l4] & PAGE_VALID)) {
        struct Page *pg = dca_alloc_pgtbl_zeroed();
        if (!pg) return (uint64_t *)0;
        uint64_t *new_l3 = (uint64_t *)Phy_To_Virt(pg->phy_address);
        pgd[l4] = Virt_To_Phy((uint64_t)new_l3) | PAGE_USER_PGD;
        ctx->new_l3_page = pg;
        ctx->created_l3 = 1;
    }
    uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l4] & PAGE_4K_MASK);

    ctx->saved_l3 = pud[l3];
    if (!(pud[l3] & PAGE_VALID)) {
        struct Page *pg = dca_alloc_pgtbl_zeroed();
        if (!pg) return (uint64_t *)0;
        uint64_t *new_l2 = (uint64_t *)Phy_To_Virt(pg->phy_address);
        pud[l3] = Virt_To_Phy((uint64_t)new_l2) | PAGE_USER_PUD;
        ctx->new_l2_page = pg;
        ctx->created_l2 = 1;
    }
    uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[l3] & PAGE_4K_MASK);

    uint64_t l2val = pmd[l2];
    ctx->saved_l2 = l2val;
    if (l2val & PAGE_HUGE) {
        struct Page *pg = dca_alloc_pgtbl_zeroed();
        if (!pg) return (uint64_t *)0;
        uint64_t *new_pte = (uint64_t *)Phy_To_Virt(pg->phy_address);
        uint64_t base = l2val & ~(uint64_t)0x1FFFFFULL;
        uint64_t perm_bits = l2val & (0x7FULL | 0x100ULL);
        uint64_t pat_bit   = (l2val & 0x1000ULL) >> 5;
        uint64_t xd_bit    = l2val & 0x8000000000000000ULL;
        uint64_t leaf_perm = perm_bits | pat_bit | xd_bit;
        for (int i = 0; i < 512; i++)
            new_pte[i] = (base + ((uint64_t)i << 12)) | leaf_perm;
        uint64_t table_perm = (l2val & (0x7FULL | 0x100ULL))
                            | (l2val & 0x8000000000000000ULL);
        ctx->saved_pmd = l2val;
        pmd[l2] = Virt_To_Phy((uint64_t)new_pte) | table_perm;
        ctx->new_pte_page = pg;
        ctx->split_2m = 1;
    } else if (!(l2val & PAGE_VALID)) {
        struct Page *pg = dca_alloc_pgtbl_zeroed();
        if (!pg) return (uint64_t *)0;
        uint64_t *new_pte = (uint64_t *)Phy_To_Virt(pg->phy_address);
        pmd[l2] = Virt_To_Phy((uint64_t)new_pte) | PAGE_USER_PUD;
        ctx->new_pte_page = pg;
        ctx->created_pte = 1;
    }
    uint64_t *pte = (uint64_t *)Phy_To_Virt(pmd[l2] & PAGE_4K_MASK);

    ctx->leaf_slot = &pte[l1];
    return ctx->leaf_slot;
}

// Free everything allocated by dca_ensure_pt and put the parent
// slots back.  Leaf slot is restored by the caller (it saved the
// original value).  Order: deepest first, then walk up.
static void dca_restore_pt(struct dca_map_ctx *ctx)
{
    if (!ctx || ctx->va == 0) return;
    uint64_t l4 = (ctx->va >> 39) & 0x1FF;
    uint64_t l3 = (ctx->va >> 30) & 0x1FF;
    uint64_t l2 = (ctx->va >> 21) & 0x1FF;
    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)arch_get_page_table());

    if (ctx->split_2m) {
        // Restore the original 2MB PDE
        uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l4] & PAGE_4K_MASK);
        uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[l3] & PAGE_4K_MASK);
        pmd[l2] = ctx->saved_pmd;
        if (ctx->new_pte_page) free_pages(ctx->new_pte_page, 1);
        ctx->new_pte_page = (struct Page *)0;
        ctx->split_2m = 0;
    } else if (ctx->created_pte) {
        // We created a fresh pte with nothing in it.  Free it.
        uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l4] & PAGE_4K_MASK);
        uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[l3] & PAGE_4K_MASK);
        pmd[l2] = 0;  // unmap before freeing the page
        if (ctx->new_pte_page) free_pages(ctx->new_pte_page, 1);
        ctx->new_pte_page = (struct Page *)0;
        ctx->created_pte = 0;
    }

    if (ctx->created_l2) {
        uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l4] & PAGE_4K_MASK);
        pud[l3] = ctx->saved_l3;
        if (ctx->new_l2_page) free_pages(ctx->new_l2_page, 1);
        ctx->new_l2_page = (struct Page *)0;
        ctx->created_l2 = 0;
    }

    if (ctx->created_l3) {
        pgd[l4] = ctx->saved_l4;
        if (ctx->new_l3_page) free_pages(ctx->new_l3_page, 1);
        ctx->new_l3_page = (struct Page *)0;
        ctx->created_l3 = 0;
    }
}

// ── The test ───────────────────────────────────────────────
int deep_copy_argv_selftest_empty(void)
{
    serial_printk("[selftest] deep_copy_argv: start\n");

    // 1. Live CR3 (init_task context — current->mm->pgdir == NULL at
    //    selftest time, so syscall_check_user_range is fail-closed).
    uint64_t *cur_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)arch_get_page_table());
    if (!cur_pml4) SELFTEST_FAIL_AT("arch_get_page_table returned NULL");

    // 2. Map a fresh user 4KB page at 0x600000.
    struct dca_map_ctx cta = {0};
    uint64_t *slot = dca_ensure_pt(cur_pml4, 0x600000, &cta);
    if (!slot) SELFTEST_FAIL_AT("dca_ensure_pt(0x600000) returned NULL");
    uint64_t saved_leaf = *slot;

    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) {
        dca_restore_pt(&cta);
        SELFTEST_FAIL_AT("alloc_pages returned NULL");
    }
    // Zero the freshly-allocated physical page (alloc_pages doesn't
    // guarantee zero init).
    memset((void *)Phy_To_Virt(pg->phy_address), 0, PAGE_4K_SIZE);

    *slot = pg->phy_address | PAGE_USER_PTE;
    arch_flush_tlb_page(0x600000);

    // 3. Switch to user-mode addr_limit and write {NULL} to the page.
    uint64_t saved_limit = current->addr_limit;
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    uint64_t empty_arr[1] = { 0 };   // {NULL}
    ssize_t wrc = copy_to_user_ft((void *)0x600000, empty_arr, sizeof(empty_arr));
    current->addr_limit = saved_limit;
    if (wrc != (ssize_t)sizeof(empty_arr)) {
        *slot = saved_leaf;
        arch_flush_tlb_page(0x600000);
        dca_restore_pt(&cta);
        free_pages(pg, 1);
        SELFTEST_FAIL_AT("copy_to_user_ft returned %ld (expected %lu)",
                         (long)wrc, (unsigned long)sizeof(empty_arr));
    }

    // 4. Call deep_copy_argv with the user-mode {NULL} array.
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    char **kargv = NULL;
    int64_t rc = deep_copy_argv((const char *const *)0x600000, &kargv);
    current->addr_limit = saved_limit;

    // 5. Assertions: rc == 0 (NOT -E2BIG), kargv non-NULL,
    //    kargv[0] == NULL (the empty-array terminator).
    if (rc != 0) {
        serial_printk("[selftest] deep_copy_argv: FAIL rc=%ld (expected 0, "
                     "E2BIG=%d EFAULT=%d)\n",
                     (long)rc, -E2BIG, -EFAULT);
        if (kargv) dca_free(kargv);
        *slot = saved_leaf;
        arch_flush_tlb_page(0x600000);
        dca_restore_pt(&cta);
        free_pages(pg, 1);
        return -1;
    }
    if (kargv == NULL) {
        serial_printk("[selftest] deep_copy_argv: FAIL kargv == NULL after rc=0\n");
        *slot = saved_leaf;
        arch_flush_tlb_page(0x600000);
        dca_restore_pt(&cta);
        free_pages(pg, 1);
        return -1;
    }
    if (kargv[0] != NULL) {
        serial_printk("[selftest] deep_copy_argv: FAIL kargv[0]=%p (expected NULL)\n",
                     (void *)kargv[0]);
        dca_free(kargv);
        *slot = saved_leaf;
        arch_flush_tlb_page(0x600000);
        dca_restore_pt(&cta);
        free_pages(pg, 1);
        return -1;
    }

    // 6. Cleanup.
    dca_free(kargv);
    *slot = saved_leaf;
    arch_flush_tlb_page(0x600000);
    dca_restore_pt(&cta);
    free_pages(pg, 1);

    serial_printk("[selftest] deep_copy_argv: PASS\n");
    return 0;
}

// ── Over-cap regression (commit 26be52e side-effect) ─────────
//
// Bug under test (post-26be52e regression): commit
//   26be52e fix(deep_copy_argv): accept explicit empty argv/envp ({NULL})
// replaced the defensive `if (count == 0) return -E2BIG;` with a comment
// justifying its removal.  But that single check was serving TWO purposes
// at once:
//
//   1. Rejecting legitimate empty arrays — the case the commit meant to
//      fix (POSIX requires argv={NULL} to be accepted).
//   2. Rejecting over-cap arrays (argv with more than MAX_ARGV=128 valid
//      pointers followed by NULL) — silently broken by the removal:
//
//      Phase 1 loops i = 0..MAX_ARGV (129 iterations).  A 129-entry argv
//      fills all 129 slots with valid pointers and exits the loop WITHOUT
//      seeing the NULL terminator (which lives at slot 129, outside the
//      loop's scan range).  count stays 0, Phase 2/3 then build a {NULL}
//      kernel array — the over-cap argv is silently accepted as an empty
//      one.  The contract comment in trap.c (line 1005) explicitly
//      requires `element count > MAX_ARGV → -E2BIG`.
//
// User-visible effect (found via OS01 systest running on master
// 01c96a8, exit code 42): test_exec_hostile_argv case 3 builds
// argv_many with 130 valid pointers and calls
//   exec("/bin/spin", argv_many, NULL)
// expecting r < 0.  Instead, exec("/bin/spin") succeeds, the current
// process image is replaced with /bin/spin, and spin's `return 42` from
// main produces exit code 42 — no [FAIL] is printed (exec never
// returns), no fail_count is incremented, and the systest loop never
// runs any test after exec_hostile_argv.
//
// This test:
//   1. Splits the boot kernel's 2MB PDEs and maps two fresh 4KB user
//      pages: 0x600000 (the argv array) and 0x601000 (backing storage
//      for the 129 single-character strings).
//   2. Writes 129 valid non-NULL string pointers + one NULL terminator
//      to 0x600000, and 'a\0' to each of the 129 backing strings at
//      0x601000.
//   3. Calls deep_copy_argv() with the array pointer.
//   4. Asserts rc == -E2BIG (over-cap rejection).
//
// Pre-fix:  rc == 0 (silently accepted as empty)  → FAIL
// Post-fix: rc == -E2BIG                            → PASS
int deep_copy_argv_selftest_overcap(void)
{
    serial_printk("[selftest] deep_copy_argv_overcap: start\n");

    // 1. Live CR3
    uint64_t *cur_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)arch_get_page_table());
    if (!cur_pml4) SELFTEST_FAIL_AT("arch_get_page_table returned NULL");

    // 2. Map ONE 4 KiB user page at 0x1000000 (1 MiB, well clear of the
    //    kernel's stack-guard region at 0x600000) and use it for both
    //    the argv array (slots [0..129], 130*8=1040 bytes) and the
    //    backing string storage (each entry i points to
    //    0x1000000 + 0x800 + i).  Single-page setup mirrors the
    //    existing _empty test and avoids the 2MB PDE shared-PTE-page
    //    subtleties that the two-page version surfaced.
    #define DCA_VA    0x1000000UL
    #define DCA_STR   (DCA_VA + 0x800)
    struct dca_map_ctx cta = {0};
    uint64_t *slot = dca_ensure_pt(cur_pml4, DCA_VA, &cta);
    if (!slot) SELFTEST_FAIL_AT("dca_ensure_pt(0x1000000) returned NULL");
    uint64_t saved_leaf = *slot;
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) {
        dca_restore_pt(&cta);
        SELFTEST_FAIL_AT("alloc_pages returned NULL");
    }
    memset((void *)Phy_To_Virt(pg->phy_address), 0, PAGE_4K_SIZE);
    *slot = pg->phy_address | PAGE_USER_PTE;
    arch_flush_tlb_page(DCA_VA);

    // 3. Switch to user-mode addr_limit and populate the page.
    uint64_t saved_limit = current->addr_limit;
    current->addr_limit = 0x00007FFFFFFFFFFFULL;

    // Strings at 0x1000800..0x1000881 (129 single-byte strings, 'a\0' each).
    // Heap-allocate the staging buffers so the function's stack frame
    // stays small (1040+130 bytes would push the stack-protector
    // canary far from RSP and expose it to setjmp/longjmp interactions
    // during the deep_copy_argv call).
    char *fill = (char *)kmalloc(130);
    uint64_t *ptrs = (uint64_t *)kmalloc(130 * 8);
    if (!fill || !ptrs) {
        if (fill) kfree(fill);
        if (ptrs) kfree(ptrs);
        current->addr_limit = saved_limit;
        *slot = saved_leaf; arch_flush_tlb_page(DCA_VA);
        dca_restore_pt(&cta); free_pages(pg, 1);
        SELFTEST_FAIL_AT("kmalloc(fill or ptrs) returned NULL");
    }
    for (int i = 0; i < 129; i++) {
        fill[i * 2] = 'a';
        fill[i * 2 + 1] = '\0';
    }
    ssize_t swrc = copy_to_user_ft((void *)DCA_STR, fill, 130);
    if (swrc != 130) {
        current->addr_limit = saved_limit;
        kfree(fill); kfree(ptrs);
        *slot = saved_leaf; arch_flush_tlb_page(DCA_VA);
        dca_restore_pt(&cta); free_pages(pg, 1);
        SELFTEST_FAIL_AT("copy_to_user_ft(strings) rc=%ld", (long)swrc);
    }

    // argv at 0x1000000: 129 pointers (each to its own 'a\0' string at
    // 0x1000800+i) + NULL terminator at slot 129.
    for (int i = 0; i < 129; i++) ptrs[i] = DCA_STR + (uint64_t)i;
    ptrs[129] = 0;
    ssize_t pwrc = copy_to_user_ft((void *)DCA_VA, ptrs, 130 * 8);
    if (pwrc != 130 * 8) {
        current->addr_limit = saved_limit;
        kfree(fill); kfree(ptrs);
        *slot = saved_leaf; arch_flush_tlb_page(DCA_VA);
        dca_restore_pt(&cta); free_pages(pg, 1);
        SELFTEST_FAIL_AT("copy_to_user_ft(argv) rc=%ld (expected %d)",
                         (long)pwrc, 130 * 8);
    }

    // 4. Call deep_copy_argv with the 129-entry argv (over-cap by 1).
    char **kargv = NULL;
    int64_t rc = deep_copy_argv((const char *const *)DCA_VA, &kargv);
    current->addr_limit = saved_limit;

    // 5. Cleanup.  The post-fix code path rejects before any kmalloc;
    //    defend against a pre-fix path that may have built a kargv.
    kfree(fill);
    kfree(ptrs);
    if (kargv) dca_free(kargv);
    *slot = saved_leaf;
    arch_flush_tlb_page(DCA_VA);
    dca_restore_pt(&cta);
    free_pages(pg, 1);

    // 6. Assert rc == -E2BIG.
    if (rc != -E2BIG) {
        serial_printk("[selftest] deep_copy_argv_overcap: FAIL rc=%ld "
                     "(expected -E2BIG=%d)\n",
                     (long)rc, -E2BIG);
        return -1;
    }
    serial_printk("[selftest] deep_copy_argv_overcap: PASS\n");
    return 0;
}

#endif // OS01_SELFTEST