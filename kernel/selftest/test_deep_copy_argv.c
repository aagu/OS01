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
#ifdef OS01_SELFTEST
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
#endif

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