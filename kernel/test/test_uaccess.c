// kernel/test/test_uaccess.c — kernel selftest for the uaccess primitives
// (Tasks 1-2).  This is the make-or-break gate BEFORE any syscall-site
// wiring: it must deterministically prove (a) the cross-level walker,
// (b) the _ft longjmp fault-recovery path, (c) cross-page correctness
// with non-contiguous physical pages, (d) the _ft_res cleanup callback.
//
// Boot context (init_task, current->mm == &init_mm): init_mm.pml4 is
// NOT set (task.h:232 only initializes .lock) — syscall_check_user_range
// returns false (fail-closed) here.  copy_*_ft is parameterized by the
// live CR3 (arch_get_page_table) and current->addr_limit; we override
// addr_limit temporarily to the user value around each longjmp call.
//
// The page-table manipulation at 0x600000/0x601000 (live CR3) uses
// hierarchy-probing helpers (ensure_pt/restore_pt) so it works whether
// the region is unmapped or a 2MB huge PDE — restore is LIFO and runs
// on EVERY exit path including before any failing assert.
//
// Registered in selftest_run_all() under the OS01_SELFTEST gate.

#include <kernel/selftest.h>
#include <kernel/uaccess.h>
#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/task.h>
#include <kernel/printk.h>
#include <kernel/arch/mmu.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

// Page-flag alias (kernel header uses PAGE_U_S; the brief uses PAGE_USER).
#ifndef PAGE_USER
#define PAGE_USER PAGE_U_S
#endif

// ── Test helpers ─────────────────────────────────────────────
//
// SELFTEST_ASSERT(cond): prints a short reason on failure (so a boot-
// hang points at the right line) and returns -1 from the enclosing
// test fn.  Matches the pattern used in test_pgrp_signal.c.
#define SELFTEST_ASSERT(cond)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            serial_printk("[selftest] uaccess: FAIL @ %s:%d: %s\n",         \
                __FILE__, __LINE__, #cond);                                 \
            return -1;                                                      \
        }                                                                   \
    } while (0)

#define SELFTEST_FAIL_AT(fmt, ...)                                         \
    do {                                                                    \
        serial_printk("[selftest] uaccess: FAIL @ %s:%d: " fmt "\n",       \
            __FILE__, __LINE__, ##__VA_ARGS__);                             \
        return -1;                                                          \
    } while (0)

// ── Synthetic pml4 builder (walker-only — copy_*_ft goes via CR3) ─
//
// g_pml4 is a fully-synthetic 4-level table wired with the rows the
// walker test asserts against.  At any given moment g_pml4[0] points
// at one of several "chains" — each row of the spec's walker table
// installs the appropriate chain (then leaves it in place for the
// duration of its test).  All rows use VA 0x400000 (or 0x800000 for
// the 2MB test) so the per-row swap is the only variable.
//
//   chain_rw   4KB user+RW              (read+write OK)
//   chain_2m   2MB huge user+RW
//   chain_uss0 4KB leaf U/S=1, upper U/S=0   (walker must reject)
//   chain_rw0  4KB leaf RW=1, upper RW=0    (read OK, write rejected)
//   chain_ro   4KB user+RO                  (read OK, write rejected)
//
// 0x500000 has no mapping (no chain sets l2=2) — used for the
// unmapped-VA test.

static uint64_t *g_pml4;
static struct Page *g_pml4_page;

// Per-chain table pointers (virtual addresses).
static uint64_t *chain_rw_pml3,   *chain_rw_pml2,   *chain_rw_pml1;
static uint64_t *chain_2m_pml3,   *chain_2m_pml2;
static uint64_t *chain_uss0_pml3, *chain_uss0_pml2, *chain_uss0_pml1;
static uint64_t *chain_rw0_pml3,  *chain_rw0_pml2,  *chain_rw0_pml1;
static uint64_t *chain_ro_pml3,   *chain_ro_pml2,   *chain_ro_pml1;

// Matching struct Page* for each chain table (so we can free_pages
// the 2 MB blocks at end of selftest instead of leaking them).
static struct Page *chain_rw_pml3_page,   *chain_rw_pml2_page,   *chain_rw_pml1_page;
static struct Page *chain_2m_pml3_page,   *chain_2m_pml2_page;
static struct Page *chain_uss0_pml3_page, *chain_uss0_pml2_page, *chain_uss0_pml1_page;
static struct Page *chain_rw0_pml3_page,  *chain_rw0_pml2_page,  *chain_rw0_pml1_page;
static struct Page *chain_ro_pml3_page,   *chain_ro_pml2_page,   *chain_ro_pml1_page;

// Per-chain leaf 4 KB pages (separate alloc_pages calls; also need
// freeing to avoid leaking the 2 MB block each was carved from).
static struct Page *chain_rw_leaf_page;
static struct Page *chain_2m_leaf_page;
static struct Page *chain_uss0_leaf_page;
static struct Page *chain_rw0_leaf_page;
static struct Page *chain_ro_leaf_page;

// Allocate a zeroed 2 MB block via alloc_pages() and return its
// kernel-mapped VA.  Also writes back the struct Page* via *out_pg
// so the caller can free_pages() the block later.  Symmetric with
// restore_pt() which calls free_pages(*out_pg, 1).
static uint64_t *alloc_pgtbl_page_zeroed(struct Page **out_pg)
{
    *out_pg = (struct Page *)0;
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) return (uint64_t *)0;
    uint64_t *v = (uint64_t *)Phy_To_Virt(pg->phy_address);
    memset(v, 0, PAGE_4K_SIZE);
    *out_pg = pg;
    return v;
}

// Install chain_X as g_pml4[0] for the duration of a test.
static void use_chain(uint64_t *pml3)
{
    g_pml4[0] = Virt_To_Phy((uint64_t)pml3) | PAGE_USER_GDT;
}

// Build a small synthetic pml4 wired with the rows the walker tests
// exercise.  See header comment above for the chain layout.
static void build_synthetic_pml4(void)
{
    g_pml4 = alloc_pgtbl_page_zeroed(&g_pml4_page);

    // chain_rw: 4KB user+RW at VA 0x400000 (l4=0,l3=0,l2=2,l1=0)
    {
        chain_rw_pml3 = alloc_pgtbl_page_zeroed(&chain_rw_pml3_page);
        chain_rw_pml2 = alloc_pgtbl_page_zeroed(&chain_rw_pml2_page);
        chain_rw_pml1 = alloc_pgtbl_page_zeroed(&chain_rw_pml1_page);
        chain_rw_pml3[0] = Virt_To_Phy((uint64_t)chain_rw_pml2) | PAGE_USER_Dir;
        chain_rw_pml2[2] = Virt_To_Phy((uint64_t)chain_rw_pml1) | PAGE_USER_Dir;
        chain_rw_leaf_page = alloc_pages(ZONE_NORMAL, 1, 0);
        chain_rw_pml1[0] = chain_rw_leaf_page->phy_address | PAGE_USER_4K;
    }

    // chain_2m: 2MB huge user+RW at VA 0x800000 (l4=0,l3=0,l2=4)
    {
        chain_2m_pml3 = alloc_pgtbl_page_zeroed(&chain_2m_pml3_page);
        chain_2m_pml2 = alloc_pgtbl_page_zeroed(&chain_2m_pml2_page);
        chain_2m_pml3[0] = Virt_To_Phy((uint64_t)chain_2m_pml2) | PAGE_USER_Dir;
        chain_2m_leaf_page = alloc_pages(ZONE_NORMAL, 1, 0);
        chain_2m_pml2[4] = (chain_2m_leaf_page->phy_address & ~(uint64_t)0x1FFFFFULL) | PAGE_USER_Page;
    }

    // chain_uss0: 4KB leaf U/S=1, upper U/S=0 — walker rejects
    {
        chain_uss0_pml3 = alloc_pgtbl_page_zeroed(&chain_uss0_pml3_page);
        chain_uss0_pml2 = alloc_pgtbl_page_zeroed(&chain_uss0_pml2_page);
        chain_uss0_pml1 = alloc_pgtbl_page_zeroed(&chain_uss0_pml1_page);
        chain_uss0_pml3[0] = Virt_To_Phy((uint64_t)chain_uss0_pml2)
                           | PAGE_KERNEL_Dir;   // U/S=0 here
        chain_uss0_pml2[2] = Virt_To_Phy((uint64_t)chain_uss0_pml1) | PAGE_USER_Dir;
        chain_uss0_leaf_page = alloc_pages(ZONE_NORMAL, 1, 0);
        chain_uss0_pml1[0] = chain_uss0_leaf_page->phy_address | PAGE_USER_4K;   // leaf U/S=1
    }

    // chain_rw0: 4KB leaf RW=1, upper RW=0 — read OK, write rejected
    {
        chain_rw0_pml3 = alloc_pgtbl_page_zeroed(&chain_rw0_pml3_page);
        chain_rw0_pml2 = alloc_pgtbl_page_zeroed(&chain_rw0_pml2_page);
        chain_rw0_pml1 = alloc_pgtbl_page_zeroed(&chain_rw0_pml1_page);
        chain_rw0_pml3[0] = Virt_To_Phy((uint64_t)chain_rw0_pml2) | PAGE_USER_Dir;
        chain_rw0_pml2[2] = Virt_To_Phy((uint64_t)chain_rw0_pml1)
                          | (PAGE_USER_Dir & ~(uint64_t)PAGE_R_W); // upper RW=0
        chain_rw0_leaf_page = alloc_pages(ZONE_NORMAL, 1, 0);
        chain_rw0_pml1[0] = chain_rw0_leaf_page->phy_address | PAGE_USER_4K;    // leaf RW=1
    }

    // chain_ro: 4KB user+RO at VA 0x400000 — read OK, write rejected
    {
        chain_ro_pml3 = alloc_pgtbl_page_zeroed(&chain_ro_pml3_page);
        chain_ro_pml2 = alloc_pgtbl_page_zeroed(&chain_ro_pml2_page);
        chain_ro_pml1 = alloc_pgtbl_page_zeroed(&chain_ro_pml1_page);
        chain_ro_pml3[0] = Virt_To_Phy((uint64_t)chain_ro_pml2) | PAGE_USER_Dir;
        chain_ro_pml2[2] = Virt_To_Phy((uint64_t)chain_ro_pml1) | PAGE_USER_Dir;
        chain_ro_leaf_page = alloc_pages(ZONE_NORMAL, 1, 0);
        chain_ro_pml1[0] = chain_ro_leaf_page->phy_address | PAGE_USER_4K_RO;  // RO leaf
    }
    // 0x500000 (l2=2) is intentionally NOT mapped in any chain (the
    // unmapped test runs before use_chain is called).
}

// ── Live-CR3 hierarchy-probing helpers ───────────────────────
//
// For 0x600000/0x601000 the boot page table might:
//   - have no entries in the user half (pml4[0] = 0) — we must create
//     pml3 -> pml2 -> pml1 (alloc + write + record original = 0)
//   - have a 2MB huge PDE at l2[3] (PAGE_PS set) — we must SPLIT it:
//     build a fresh pml1 with all 512 4KB subpages mapping the same
//     2 MB (preserving access for any code that happened to be using
//     a VA in 0x600000..0x7fffff), then replace the PDE with a
//     non-leaf table pointer.  Record the original PDE for restore.
//
// These helpers never touch a leaf they didn't create: they only ever
// probe/save/modify the intermediate slots needed to reach a fresh
// leaf, and they record every modification so restore is exact.

struct test_map_ctx {
    uint64_t  va;
    uint64_t  saved_l4, saved_l3, saved_l2;   // originals (0 = absent, allocated)
    uint64_t  saved_pde;                      // original 2MB PDE if we split
    uint64_t  *new_l3, *new_l2, *new_pml1;    // test-only tables we created
    struct Page *new_l3_page, *new_l2_page, *new_pml1_page;  // matching 2 MB blocks for free_pages()
    uint64_t  *leaf_slot;                     // pointer into a table to leaf PTE
    int       created_l3;
    int       created_l2;
    int       created_pml1;
    int       split_2m;
};

// Walk l4->l3->l2, creating intermediates as needed, then split the
// l2 PDE if it's a 2MB huge page.  Return the leaf slot pointer
// (zero on alloc failure).
static uint64_t *ensure_pt(uint64_t *pml4, uint64_t va, struct test_map_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->va = va;
    uint64_t l4 = (va >> 39) & 0x1FF;
    uint64_t l3 = (va >> 30) & 0x1FF;
    uint64_t l2 = (va >> 21) & 0x1FF;
    uint64_t l1 = (va >> 12) & 0x1FF;

    // ── l4 ──────────────────────────────────────────────────────
    ctx->saved_l4 = pml4[l4];
    if (!(pml4[l4] & PAGE_Present)) {
        ctx->new_l3 = alloc_pgtbl_page_zeroed(&ctx->new_l3_page);
        if (!ctx->new_l3) return (uint64_t *)0;
        pml4[l4] = Virt_To_Phy((uint64_t)ctx->new_l3) | PAGE_USER_GDT;
        ctx->created_l3 = 1;
    }
    uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);

    // ── l3 ──────────────────────────────────────────────────────
    ctx->saved_l3 = pml3[l3];
    if (!(pml3[l3] & PAGE_Present)) {
        ctx->new_l2 = alloc_pgtbl_page_zeroed(&ctx->new_l2_page);
        if (!ctx->new_l2) return (uint64_t *)0;
        pml3[l3] = Virt_To_Phy((uint64_t)ctx->new_l2) | PAGE_USER_Dir;
        ctx->created_l2 = 1;
    }
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);

    // ── l2: split 2MB huge or create table ──────────────────────
    uint64_t l2val = pml2[l2];
    ctx->saved_l2 = l2val;
    if (l2val & PAGE_PS) {
        // 2MB huge PDE — split into 512 4KB PTEs that preserve the
        // original mapping (all 511 sibling 4KB pages remain reachable).
        ctx->new_pml1 = alloc_pgtbl_page_zeroed(&ctx->new_pml1_page);
        if (!ctx->new_pml1) return (uint64_t *)0;
        uint64_t base = l2val & ~(uint64_t)0x1FFFFFULL;
        // Leaf PTE perms: P,RW,U/S, PWT,PCD,A,D, G(8) preserved; PS(7)=0;
        // PAT(12)->PTE(7); XD(63) preserved.  Strip physical bits.
        uint64_t perm_bits = l2val & (0x7FULL | 0x100ULL);
        uint64_t pat_bit   = (l2val & 0x1000ULL) >> 5;
        uint64_t xd_bit    = l2val & 0x8000000000000000ULL;
        uint64_t leaf_perm = perm_bits | pat_bit | xd_bit;
        for (int i = 0; i < 512; i++) {
            ctx->new_pml1[i] = (base + ((uint64_t)i << 12)) | leaf_perm;
        }
        // Non-leaf table entry: present, RW, U/S (so user walks succeed).
        // PS(7)=0, no PAT, no physical-address bits.
        uint64_t table_perm = (l2val & (0x7FULL | 0x100ULL))
                            | (l2val & 0x8000000000000000ULL);
        ctx->saved_pde = l2val;
        pml2[l2] = Virt_To_Phy((uint64_t)ctx->new_pml1) | table_perm;
        ctx->created_pml1 = 1;
        ctx->split_2m = 1;
    } else if (!(l2val & PAGE_Present)) {
        ctx->new_pml1 = alloc_pgtbl_page_zeroed(&ctx->new_pml1_page);
        if (!ctx->new_pml1) return (uint64_t *)0;
        pml2[l2] = Virt_To_Phy((uint64_t)ctx->new_pml1) | PAGE_USER_Dir;
        ctx->created_pml1 = 1;
    }
    uint64_t *pml1 = (uint64_t *)Phy_To_Virt(pml2[l2] & PAGE_4K_MASK);

    ctx->leaf_slot = &pml1[l1];
    return ctx->leaf_slot;
}

// Restore what ensure_pt created.  Call in LIFO order across ctxs.
// Within a single ctx: leaf slot was already restored by the caller
// (saved_a/b).  We only need to: free any test-only tables and put
// the parent slots back to their original values.
static void restore_pt(struct test_map_ctx *ctx)
{
    if (!ctx || ctx->va == 0) return;
    uint64_t l4 = (ctx->va >> 39) & 0x1FF;
    uint64_t l3 = (ctx->va >> 30) & 0x1FF;
    uint64_t l2 = (ctx->va >> 21) & 0x1FF;

    // Re-derive the l4 entry — we always modify pml4[0] (l4=0) for
    // the 0x600000/0x601000 range.
    uint64_t *pml4 = (uint64_t *)Phy_To_Virt((uint64_t)arch_get_page_table());

    if (ctx->split_2m) {
        // Restore the original 2MB PDE
        uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);
        uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);
        pml2[l2] = ctx->saved_pde;
        // Free the test-only pml1 we created during split.  alloc_pgtbl_page_zeroed
        // used alloc_pages(ZONE_NORMAL, 1, ...) which returns a 2 MB block;
        // free_4k_page() only handles subpage_pool 4 KB slots, so we must
        // use free_pages(pg, 1) to release the underlying 2 MB block.
        if (ctx->new_pml1_page) free_pages(ctx->new_pml1_page, 1);
        ctx->new_pml1 = (uint64_t *)0;
        ctx->new_pml1_page = (struct Page *)0;
        ctx->split_2m = 0;
    } else if (ctx->created_pml1) {
        // We created a fresh pml1 with nothing in it.  Free it (2 MB block).
        if (ctx->new_pml1_page) free_pages(ctx->new_pml1_page, 1);
        ctx->new_pml1 = (uint64_t *)0;
        ctx->new_pml1_page = (struct Page *)0;
        ctx->created_pml1 = 0;
    }

    // Restore l3 slot if we created the l2 table it points to.
    if (ctx->created_l2) {
        uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);
        pml3[l3] = ctx->saved_l3;
        if (ctx->new_l2_page) free_pages(ctx->new_l2_page, 1);
        ctx->new_l2 = (uint64_t *)0;
        ctx->new_l2_page = (struct Page *)0;
        ctx->created_l2 = 0;
    }

    // Restore l4 slot if we created the l3 table it points to.
    if (ctx->created_l3) {
        pml4[l4] = ctx->saved_l4;
        if (ctx->new_l3_page) free_pages(ctx->new_l3_page, 1);
        ctx->new_l3 = (uint64_t *)0;
        ctx->new_l3_page = (struct Page *)0;
        ctx->created_l3 = 0;
    }

    // Flush TLB for the VA range we may have touched.
    arch_flush_tlb_page(ctx->va);
    arch_flush_tlb_page(ctx->va + 0x1000);
}

// ── selftest entry point ──────────────────────────────────────
static int cleanup_ran;
static void cb_cleanup(void *arg)
{
    (void)arg;
    cleanup_ran = 1;
}

int selftest_uaccess(void)
{
    serial_printk("[selftest] uaccess: start\n");

    // ── Step 1: walker-only tests on a synthetic pml4 ──────────
    build_synthetic_pml4();
    if (!g_pml4) SELFTEST_FAIL_AT("build_synthetic_pml4 returned NULL");

    // VA 0x500000 (unmapped — no chain sets l2=2)
    SELFTEST_ASSERT(!arch_user_range_accessible(g_pml4, 0x500000, 4096, false));

    // 0x400000 4KB present+user+RW: read & write OK (chain_rw)
    use_chain(chain_rw_pml3);
    SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x400000, 4096, false));
    SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x400000, 4096, true));

    // 0x400000 4KB user+RO (chain_ro): read OK, write rejected
    use_chain(chain_ro_pml3);
    SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x400000, 4096, false));
    SELFTEST_ASSERT(!arch_user_range_accessible(g_pml4, 0x400000, 4096, true));

    // Upper U/S=0 (chain_uss0): leaf U/S=1 but pml3 has U/S=0 -> reject
    use_chain(chain_uss0_pml3);
    SELFTEST_ASSERT(!arch_user_range_accessible(g_pml4, 0x400000, 4096, false));

    // Upper RW=0 (chain_rw0): read OK, write rejected
    use_chain(chain_rw0_pml3);
    SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x400000, 4096, false));
    SELFTEST_ASSERT(!arch_user_range_accessible(g_pml4, 0x400000, 4096, true));

    // 2MB huge at 0x800000 (chain_2m)
    use_chain(chain_2m_pml3);
    SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x800000, 4096, false));
    SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x800000, 4096, true));

    // Cross-two-pages: 0x600ff0..0x601010 spans the boundary at
    // 0x601000.  Build a dedicated chain for this row (it needs l2=3).
    {
        struct Page *pml3_page, *pml2_page, *pml1_page;
        uint64_t *pml3 = alloc_pgtbl_page_zeroed(&pml3_page);
        uint64_t *pml2 = alloc_pgtbl_page_zeroed(&pml2_page);
        uint64_t *pml1 = alloc_pgtbl_page_zeroed(&pml1_page);
        pml3[0] = Virt_To_Phy((uint64_t)pml2) | PAGE_USER_Dir;
        pml2[3] = Virt_To_Phy((uint64_t)pml1) | PAGE_USER_Dir;
        struct Page *p0 = alloc_pages(ZONE_NORMAL, 1, 0);
        struct Page *p1 = alloc_pages(ZONE_NORMAL, 1, 0);
        pml1[0] = p0->phy_address | PAGE_USER_4K;
        pml1[1] = p1->phy_address | PAGE_USER_4K;
        use_chain(pml3);
        SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x600ff0, 32, false));
        SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x600ff0, 32, true));
        // Free chain (each is its own 2 MB block; free_pages(pg, 1) releases it).
        free_pages(p0, 1);
        free_pages(p1, 1);
        free_pages(pml1_page, 1);
        free_pages(pml2_page, 1);
        free_pages(pml3_page, 1);
    }

    serial_printk("[selftest] uaccess: walker OK\n");

    // ── Step 2: address-math (boot ctx: init_mm.pml4 unset, fail-closed) ─
    SELFTEST_ASSERT(!syscall_check_user_range(0, 4, true));                  // NULL
    SELFTEST_ASSERT(!syscall_check_user_range(0x1, 4, true));                // < USER_MIN_ADDR
    SELFTEST_ASSERT(!syscall_check_user_range(0xffff800000000000ULL, 4, true)); // kernel
    SELFTEST_ASSERT(!syscall_check_user_range(0x400000, (1ULL<<40), true));  // overflow
    SELFTEST_ASSERT(syscall_check_user_range(0x400000, 0, true));            // len==0
    serial_printk("[selftest] uaccess: address-math OK\n");

    // ── Step 3: longjmp path (unmapped VA -> #PF -> longjmp) ───
    // Boot context: addr_limit = 0xffff800000000000 (kernel).  Override to
    // user value so the do_page_fault gate sees cr2 < addr_limit.  The
    // boot kernel maps 0..2MB and 0x600000..0x800000 as 2MB huge PDEs,
    // so we use 0x40000000 (in the unmapped 1GB..2GB gap) for the fault.
    uint64_t saved_limit = current->addr_limit;
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    char ksrc[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                     17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
    ssize_t rc = copy_to_user_ft((void *)0x40000000, ksrc, 16);  // unmapped -> #PF -> longjmp
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(rc == -EFAULT);
    serial_printk("[selftest] uaccess: longjmp OK\n");

    // ── Step 4: live-CR3 hierarchy-probing + cross-page + _ft_res ─
    // The boot context's pml4 is the kernel pml4 (CR3).  NEVER use
    // current->mm->pml4 here — init_mm.pml4 is NULL.
    uint64_t *cur_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)arch_get_page_table());
    if (!cur_pml4) SELFTEST_FAIL_AT("arch_get_page_table returned NULL");

    struct test_map_ctx cta = {0}, ctb = {0};
    uint64_t *slot_a = ensure_pt(cur_pml4, 0x600000, &cta);
    if (!slot_a) SELFTEST_FAIL_AT("ensure_pt(0x600000) returned NULL");
    uint64_t saved_a = *slot_a;
    uint64_t *slot_b = ensure_pt(cur_pml4, 0x601000, &ctb);
    if (!slot_b) SELFTEST_FAIL_AT("ensure_pt(0x601000) returned NULL");
    uint64_t saved_b = *slot_b;

    // Allocate two genuinely non-adjacent 4KB pages by grabbing two
    // distinct 2 MB pages from the page allocator and using subpage 0
    // of each.  Two 2 MB blocks from alloc_pages are always at
    // >=2 MB apart, so pa and pb are guaranteed non-adjacent
    // (>=512 4 KB pages between them).  Free with free_pages(pg, 1)
    // — note the struct Page* is from alloc_pages, NOT a uint64 phys
    // from alloc_4k_page.
    struct Page *pgA = alloc_pages(ZONE_NORMAL, 1, 0);
    struct Page *pgB = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pgA || !pgB || pgA == pgB) {
        if (pgA) free_pages(pgA, 1);
        if (pgB) free_pages(pgB, 1);
        *slot_a = saved_a;
        *slot_b = saved_b;
        arch_flush_tlb_page(0x600000);
        arch_flush_tlb_page(0x601000);
        restore_pt(&ctb);
        restore_pt(&cta);
        SELFTEST_FAIL_AT("alloc_pages returned NULL or same 2MB block "
                         "(pgA=%p pgB=%p)", (void *)pgA, (void *)pgB);
    }
    uint64_t pa = pgA->phy_address;
    uint64_t pb = pgB->phy_address;
    // Hard gate: two distinct 2 MB blocks MUST be >=2 MB apart in
    // phys.  If this ever fires, that's a real allocator anomaly.
    if ((pa >> 12) + 1 == (pb >> 12)) {
        free_pages(pgA, 1);
        free_pages(pgB, 1);
        *slot_a = saved_a;
        *slot_b = saved_b;
        arch_flush_tlb_page(0x600000);
        arch_flush_tlb_page(0x601000);
        restore_pt(&ctb);
        restore_pt(&cta);
        SELFTEST_FAIL_AT("non-adjacency gate fired (pa=%lx pb=%lx)",
                         (unsigned long)pa, (unsigned long)pb);
    }

    // ── Step 5: cross-page no-short-count fault test ───────────
    // After ensure_pt split the 2MB PDE, BOTH pages are mapped (all
    // 512 4KB subpages of the original 2MB mapping are preserved).
    // For the fault test we explicitly zero slot_b (saving its
    // current value for restoration).
    *slot_a = pa | PAGE_USER_4K;
    arch_flush_tlb_page(0x600000);
    uint64_t saved_b_for_fault = *slot_b;   // may be a 4KB PTE if split happened
    *slot_b = 0;                            // genuinely unmap page B
    arch_flush_tlb_page(0x601000);

    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    rc = copy_to_user_ft((void *)0x600ff0, ksrc, 32);   // spans into page B
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(rc == -EFAULT);

    // ── Step 6: cross-page correctness with non-contiguous phys ─
    *slot_b = pb | PAGE_USER_4K;
    arch_flush_tlb_page(0x601000);
    uint8_t *kA = (uint8_t *)Phy_To_Virt(pa);
    uint8_t *kB = (uint8_t *)Phy_To_Virt(pb);
    memset(kA, 0xAA, 4096);
    memset(kB, 0xBB, 4096);

    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    ssize_t n = copy_to_user_ft((void *)0x600ff0, ksrc, 32);
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(n == 32);
    SELFTEST_ASSERT(memcmp(kA + 0xff0, ksrc, 16) == 0);    // first 16B in page A
    SELFTEST_ASSERT(memcmp(kB, ksrc + 16, 16) == 0);      // last 16B in page B
    serial_printk("[selftest] uaccess: cross-page OK\n");

    // ── Step 7: _ft_res cleanup callback ───────────────────────
    // On-fault: cleanup MUST run.  Success: cleanup MUST NOT run.
    // Use 0x40000000 (in the 1GB..2GB unmapped gap) for the fault path.
    cleanup_ran = 0;

    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    rc = copy_to_user_ft_res((void *)0x40000000, ksrc, 16, cb_cleanup, NULL);
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(rc == -EFAULT);
    SELFTEST_ASSERT(cleanup_ran == 1);                    // cleanup ran on fault

    cleanup_ran = 0;
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    rc = copy_to_user_ft_res((void *)0x600000, ksrc, 16, cb_cleanup, NULL);
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(rc == 16);
    SELFTEST_ASSERT(cleanup_ran == 0);                    // success: no cleanup
    serial_printk("[selftest] uaccess: _ft_res OK\n");

    // ── Step 8: strnlen_user ──────────────────────────────────
    // page-tail NUL: "AB\0" at offset 0xffc..0xffe of page A -> returns 2.
    uint8_t *kA2 = (uint8_t *)Phy_To_Virt(pa);
    kA2[0xffc] = 'A';
    kA2[0xffd] = 'B';
    kA2[0xffe] = '\0';
    arch_flush_tlb_page(0x600000);
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    int sl = strnlen_user((void *)0x600ffc, 64);
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(sl == 2);

    // no NUL within max: 64 bytes of non-NUL -> returns 64.
    memset(kA2, 'X', 4096);
    kA2[0xfff] = 'X';   // ensure last byte in page A isn't NUL
    arch_flush_tlb_page(0x600000);
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    sl = strnlen_user((void *)0x600000, 64);
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(sl == 64);

    // unmapped -> -EFAULT (use the 1GB..2GB gap)
    current->addr_limit = 0x00007FFFFFFFFFFFULL;
    sl = strnlen_user((void *)0x40000000, 16);
    current->addr_limit = saved_limit;
    SELFTEST_ASSERT(sl == -EFAULT);
    serial_printk("[selftest] uaccess: strnlen_user OK\n");

    // ── Step 9: restore page tables + free phys pages ─────────
    // Leaf slots first (saved_a/b may be 0 if there was no original leaf),
    // then LIFO restore of the two ctxs (ctb created after cta -> ctb first).
    *slot_a = saved_a;
    *slot_b = saved_b;
    arch_flush_tlb_page(0x600000);
    arch_flush_tlb_page(0x601000);
    restore_pt(&ctb);    // LIFO: later-created first
    restore_pt(&cta);
    free_pages(pgA, 1); // 2 MB block (count arg required)
    free_pages(pgB, 1);

    // ── Step 10: free the synthetic pml4 + chains + leaf pages ─
    // Each alloc_pgtbl_page_zeroed() + each chain leaf came from
    // alloc_pages(ZONE_NORMAL, 1, ...), i.e. a 2 MB block.  Free them
    // symmetrically with free_pages(pg, 1).
    free_pages(chain_rw_leaf_page,  1);
    free_pages(chain_2m_leaf_page,  1);
    free_pages(chain_uss0_leaf_page,1);
    free_pages(chain_rw0_leaf_page, 1);
    free_pages(chain_ro_leaf_page,  1);
    free_pages(chain_rw_pml1_page,  1);
    free_pages(chain_rw_pml2_page,  1);
    free_pages(chain_rw_pml3_page,  1);
    free_pages(chain_2m_pml2_page,  1);
    free_pages(chain_2m_pml3_page,  1);
    free_pages(chain_uss0_pml1_page,1);
    free_pages(chain_uss0_pml2_page,1);
    free_pages(chain_uss0_pml3_page,1);
    free_pages(chain_rw0_pml1_page, 1);
    free_pages(chain_rw0_pml2_page, 1);
    free_pages(chain_rw0_pml3_page, 1);
    free_pages(chain_ro_pml1_page,  1);
    free_pages(chain_ro_pml2_page,  1);
    free_pages(chain_ro_pml3_page,  1);
    free_pages(g_pml4_page, 1);

    serial_printk("[selftest] uaccess: PASS\n");
    return 0;
}