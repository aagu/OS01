/* kernel/arch/aarch64/memory/runtime_tree.c
 *
 * aarch64 M1 plan Task 4 — pure runtime page-table builder and strict
 * validator.
 *
 * Architecture (mirrors spec §5):
 *
 *   - 4-level 48-bit VA regime, 4 KiB granule, 40-bit IPS. TCR/MAIR
 *     are owned by head.S and never modified here. We only install
 *     L0/L1/L2 tables; L3 leaves never appear in this tree. The
 *     builder does not depend on `aarch64_pt_map_4k` or any active
 *     page-table walker — the active root is still the boot tree at
 *     this point — so descriptor encoding is done by a small private
 *     helper (`encode_table_desc`, `encode_block_desc`).
 *
 *   - L0 is the root (one page), L1 spans 512 GiB (we need at most
 *     2 of them under 1 TiB), L2 spans 1 GiB (at most 1024 of them).
 *     So at most 1 + 2 + 1024 = 1027 pages — matches the spec's
 *     AARCH64_M1_TABLE_PAGES_MAX ceiling.
 *
 *   - The builder's job is to enumerate the set of 2 MiB blocks the
 *     new tree must contain (R ∪ B ∪ D) and install each as an L2
 *     block descriptor, allocating L1/L2 tables lazily as needed.
 *     "B exact" means the B window's blocks are always installed with
 *     the executable permission (PXN=0); R/D conflict means a RAM
 *     range that overlaps the Device window is an error.
 *
 *   - The validator independently walks the tree from `tree->root_pa`
 *     in PA-ascending order and pairs each yielded (pa, desc) against
 *     an expected-set iterator that also visits in PA-ascending order.
 *     The two iterators step in lockstep; any drift between them is a
 *     contract failure. Uniqueness of intermediate tables, pool
 *     ownership, and SBZ-bit checks all live in the tree iterator;
 *     no large heap allocation is required.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <arch/aarch64/runtime_tree.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/page_table.h>
#include <memory/memory_map.h>

/* ── Spec / derived constants ───────────────────────────────── */

/* PA limit: 1 TiB. Matches AARCH64_M1_PA_LIMIT. */
#define RT_PA_LIMIT     UINT64_C(0x10000000000)
#define RT_PAGE_4K      UINT64_C(0x1000)
#define RT_PAGE_2M      UINT64_C(0x200000)

/* Index sizes. */
#define RT_L0_SHIFT     39u
#define RT_L1_SHIFT     30u
#define RT_L2_SHIFT     21u
#define RT_IDX_MASK     UINT64_C(0x1ff)
#define RT_ENTRIES      512u

/* Descriptor bits. Mirrors the private constants in page_table.c. */
#define RT_DESC_VALID   UINT64_C(0x001)          /* bit 0 */
#define RT_DESC_TABLE   UINT64_C(0x002)          /* bit 1 (table at L0/L1/L2; leaf at L3) */
#define RT_DESC_AF      UINT64_C(0x400)          /* bit 10 */
#define RT_DESC_SH_IS   UINT64_C(0x300)          /* bits [9:8] inner-shareable */
#define RT_DESC_SH_NS   UINT64_C(0x000)          /* bits [9:8] non-shareable */

/* AP[2:1] in bits [7:6]; kernel RW = 00. */
#define RT_DESC_AP_MASK     UINT64_C(0x0C0)
#define RT_DESC_AP_KRW      UINT64_C(0x000)

/* AttrIndx in bits [4:2]: 0 = Device, 1 = Normal. */
#define RT_DESC_ATTR_MASK   UINT64_C(0x01C)
#define RT_DESC_ATTR_NORMAL UINT64_C(0x004)
#define RT_DESC_ATTR_DEVICE UINT64_C(0x000)

/* Execute-never bits. */
#define RT_DESC_PXN     UINT64_C(0x02000000000000)  /* bit 53 */
#define RT_DESC_UXN     UINT64_C(0x04000000000000)  /* bit 54 */

/* L2 block PA mask: bits [39:21] = output block address. */
#define RT_DESC_L2_PA_MASK  UINT64_C(0xffffffe00000)

/* Intermediate (table) PA mask: bits [39:12]. */
#define RT_DESC_L_PA_MASK   UINT64_C(0xffffffffff000)

/* Spec constant sets (B kernel+handoff; D Device window). */
#define RT_B_BASE  UINT64_C(0x40000000)
#define RT_B_END   UINT64_C(0x40200000)
#define RT_D_BASE  UINT64_C(0x08000000)
#define RT_D_END   UINT64_C(0x0a000000)

/* ── Input validation ───────────────────────────────────────── */

/* Validate the ram list and ensure no R entry overlaps B or D. The
 * arena planner already enforces this for production paths; the
 * builder repeats the check so a forged caller can't smuggle a B or D
 * PA through ram.
 *
 * Returns 0 on success. On failure leaves *err set to the errno the
 * caller should return. */
static int rt_validate_ram(const struct MEMORY_RANGE *ram, size_t count,
                           int *err)
{
    uint64_t prev_end = 0;
    int have_prev = 0;
    size_t i;

    if (ram == NULL) { if (count != 0) { *err = -EINVAL; return -1; } return 0; }
    if (count > MEMORY_RANGE_MAX) { *err = -EINVAL; return -1; }

    for (i = 0u; i < count; ++i) {
        uint64_t s = ram[i].phys_start;
        uint64_t e = ram[i].phys_end;

        if (ram[i].type != MEMORY_TYPE_RAM) { *err = -EINVAL; return -1; }
        if ((s & (RT_PAGE_2M - 1u)) != 0u) { *err = -EINVAL; return -1; }
        if ((e & (RT_PAGE_2M - 1u)) != 0u) { *err = -EINVAL; return -1; }
        if (s >= e) { *err = -EINVAL; return -1; }
        if (e > RT_PA_LIMIT) { *err = -ERANGE; return -1; }

        /* R/B kernel-handoff exclusion: any RAM range that overlaps
         * [0x40000000, 0x40200000) is illegal — the kernel + handoff
         * window is already excluded upstream, but it is re-checked
         * here defensively. */
        if (s < RT_B_END && e > RT_B_BASE) { *err = -EINVAL; return -1; }

        /* R/D Device conflict: any RAM range that touches the
         * Device window is illegal. */
        if (s < RT_D_END && e > RT_D_BASE) { *err = -EINVAL; return -1; }

        if (have_prev && s <= prev_end) { *err = -EINVAL; return -1; }
        prev_end = e;
        have_prev = 1;
    }
    return 0;
}

/* ── Descriptor encoding ────────────────────────────────────── */

/* Minimal intermediate table descriptor — only V | TYPE_TABLE | PA.
 * Per ARM ARM D5.4.3 every other bit is SBZ at L0/L1/L2, and QEMU
 * TCG strictly faults a translation walk on a non-zero SBZ bit. */
static uint64_t rt_encode_table_desc(uint64_t pa)
{
    return RT_DESC_VALID | RT_DESC_TABLE | (pa & RT_DESC_L_PA_MASK);
}

/* Compute the L2 block descriptor for the given (pa, perm) pair. The
 * perm word is one of the three specs in the header (KERNEL_RW;
 * KERNEL_RW | EXEC; KERNEL_RW | DEVICE). The DEVICE branch never has
 * EXEC set, so we don't have to special-case DEVICE | EXEC (which the
 * page-table primitive rejects; the builder does not write a leaf
 * with that combo).
 *
 * AF is always set; Contiguous is always cleared. */
static uint64_t rt_encode_block_desc(uint64_t pa, uint32_t perm)
{
    /* AttrIndx + shareability are jointly encoded by the DEVICE bit:
     *   device → AttrIndx 0, non-shareable, no XN clearance;
     *   normal → AttrIndx 1, inner-shareable, optional EXEC clears
     *            the privilege-matching XN. */
    if (perm & AARCH64_PT_DEVICE) {
        return RT_DESC_VALID | RT_DESC_AF |
                   RT_DESC_SH_NS | RT_DESC_AP_KRW |
                   RT_DESC_ATTR_DEVICE |
                   RT_DESC_PXN | RT_DESC_UXN |
                   (pa & RT_DESC_L2_PA_MASK);
    }
    if (perm & AARCH64_PT_EXEC) {
        /* Normal + executable: clear PXN (kernel can execute),
         * keep UXN (EL0 still no execute). */
        return RT_DESC_VALID | RT_DESC_AF |
                   RT_DESC_SH_IS | RT_DESC_AP_KRW |
                   RT_DESC_ATTR_NORMAL |
                   RT_DESC_UXN |
                   (pa & RT_DESC_L2_PA_MASK);
    }
    /* Normal non-executable: both XN bits set. */
    return RT_DESC_VALID | RT_DESC_AF |
               RT_DESC_SH_IS | RT_DESC_AP_KRW |
               RT_DESC_ATTR_NORMAL |
               RT_DESC_PXN | RT_DESC_UXN |
               (pa & RT_DESC_L2_PA_MASK);
}

/* ── Expected-set enumeration ─────────────────────────────────
 *
 * The expected set is the 2 MiB blocks that MUST be present in the
 * tree. The build visits them in (B, D, R) order; the validator visits
 * them in PA-ascending order so it can lockstep with the tree
 * iterator (which also visits in PA order). */
typedef int (*rt_block_visit_t)(void *ctx, uint64_t pa, uint32_t perm);

/* (B, D, R) order. Used by the build. */
static int rt_walk_expected_blocks(const struct MEMORY_RANGE *ram, size_t count,
                                   rt_block_visit_t cb, void *ctx)
{
    uint64_t pa;
    size_t i;
    int rc;

    /* B: exact override, kernel RW + EXEC. */
    for (pa = RT_B_BASE; pa < RT_B_END; pa += RT_PAGE_2M) {
        rc = cb(ctx, pa, AARCH64_PT_KERNEL_RW | AARCH64_PT_EXEC);
        if (rc != 0) return rc;
    }

    /* D: exact override, kernel RW + DEVICE. */
    for (pa = RT_D_BASE; pa < RT_D_END; pa += RT_PAGE_2M) {
        rc = cb(ctx, pa, AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE);
        if (rc != 0) return rc;
    }

    /* R: kernel RW, never overlaps B/D (validated upstream). */
    for (i = 0u; i < count; ++i) {
        for (pa = ram[i].phys_start; pa < ram[i].phys_end; pa += RT_PAGE_2M) {
            rc = cb(ctx, pa, AARCH64_PT_KERNEL_RW);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

/* ── Build ────────────────────────────────────────────────────
 *
 * The build driver walks the expected set lazily: each (pa, perm)
 * pair triggers a (l0_idx, l1_idx, l2_idx) tuple and ensures the L1
 * and L2 intermediate tables exist (allocating via ops->alloc only
 * when a new bucket is touched). Each L0/L1/L2 page is allocated
 * exactly once and stored in a small index → PA map so subsequent
 * visits to the same bucket reuse the same table.
 */
typedef struct rt_builder {
    const struct aarch64_tree_ops *ops;
    uint64_t *root_va;
    /* PUD pages, indexed by L0 entry (0..1). For PA < 1 TiB only
     * indices 0 and 1 are ever touched. */
    uint64_t pud_pa[2];
    uint64_t *pud_va[2];
    /* PMD pages, indexed by (l0_idx * 512 + l1_idx). Each
     * (l0_idx, l1_idx) pair identifies the unique PMD page that
     * covers the 1 GiB bucket (l1_idx * 1 GiB) inside L0[l0_idx].
     * Max 2 * 512 = 1024 PMD pages. */
    uint64_t pmd_pa[1024];
    uint64_t *pmd_va[1024];
    /* Bookkeeping. */
    uint64_t first_pa;
    uint64_t last_pa;
    size_t pages_issued;
    size_t pages_cap;
} rt_builder_t;

/* Allocate a fresh 4 KiB page through ops->alloc and update the
 * builder's bookkeeping. Returns 0 on success; on failure returns the
 * negative errno the builder should propagate and leaves the
 * builder unchanged (no descriptor has been written yet). */
static int rt_alloc_one(rt_builder_t *b, uint64_t *out_pa, uint64_t **out_va)
{
    uint64_t pa = 0;
    uint64_t *va = NULL;
    int rc;

    if (b->pages_issued >= b->pages_cap) return -ENOMEM;

    rc = b->ops->alloc(b->ops->ctx, &pa, &va);
    if (rc != 0) return rc;
    if (pa == 0) return -EINVAL;
    if ((pa & (RT_PAGE_4K - 1u)) != 0u) return -EINVAL;
    if (va == NULL) return -EINVAL;

    /* Zero the page so unwritten slots read as invalid descriptors
     * (V=0). The walk-cache fault for an invalid descriptor is
     * deterministic; a stale 0xDEADBEEF would silently extend the
     * mapping set. */
    {
        volatile uint64_t *cur = (volatile uint64_t *)va;
        volatile uint64_t *end = cur + (RT_PAGE_4K / sizeof(uint64_t));
        while (cur < end) { *cur = 0; ++cur; }
    }

    if (b->pages_issued == 0u) b->first_pa = pa;
    b->last_pa = pa;
    ++b->pages_issued;

    *out_pa = pa;
    *out_va = va;
    return 0;
}

/* Find the PUD page for L0 entry l0_idx. Returns 1 if found, 0
 * otherwise. */
static int rt_find_pud(const rt_builder_t *b, uint64_t l0_idx,
                       uint64_t *out_pa, uint64_t **out_va)
{
    if (l0_idx >= 2u) return 0;
    if (b->pud_pa[l0_idx] == 0u) return 0;
    *out_pa = b->pud_pa[l0_idx];
    *out_va = b->pud_va[l0_idx];
    return 1;
}

/* Find the PMD page for (l0_idx, l1_idx). Returns 1 if found, 0
 * otherwise. */
static int rt_find_pmd(const rt_builder_t *b, uint64_t l0_idx,
                       uint64_t l1_idx, uint64_t *out_pa, uint64_t **out_va)
{
    size_t idx;
    if (l0_idx >= 2u) return 0;
    if (l1_idx >= 512u) return 0;
    idx = (size_t)l0_idx * 512u + (size_t)l1_idx;
    if (b->pmd_pa[idx] == 0u) return 0;
    *out_pa = b->pmd_pa[idx];
    *out_va = b->pmd_va[idx];
    return 1;
}

/* Allocate a fresh PUD page for L0 entry l0_idx. Stores it in the
 * builder's L1 map and writes L0[l0_idx] = table desc → PUD. */
static int rt_install_pud(rt_builder_t *b, uint64_t l0_idx)
{
    uint64_t pa = 0;
    uint64_t *va = NULL;
    int rc;

    if (l0_idx >= 2u) return -EINVAL;

    rc = rt_alloc_one(b, &pa, &va);
    if (rc != 0) return rc;
    b->pud_pa[l0_idx] = pa;
    b->pud_va[l0_idx] = va;
    b->root_va[l0_idx] = rt_encode_table_desc(pa);
    return 0;
}

/* Allocate a fresh PMD page for (l0_idx, l1_idx). Stores it in the
 * builder's L2 map and writes PUD[l1_idx] = table desc → PMD. */
static int rt_install_pmd(rt_builder_t *b, uint64_t l0_idx, uint64_t l1_idx)
{
    size_t idx;
    uint64_t pud_pa;
    uint64_t *pud_va;
    uint64_t pa = 0;
    uint64_t *va = NULL;
    int rc;

    if (l0_idx >= 2u) return -EINVAL;
    if (l1_idx >= 512u) return -EINVAL;

    rc = rt_find_pud(b, l0_idx, &pud_pa, &pud_va);
    if (rc == 0) return -EINVAL;
    rc = rt_alloc_one(b, &pa, &va);
    if (rc != 0) return rc;
    idx = (size_t)l0_idx * 512u + (size_t)l1_idx;
    b->pmd_pa[idx] = pa;
    b->pmd_va[idx] = va;
    pud_va[l1_idx] = rt_encode_table_desc(pa);
    return 0;
}

static int rt_install_block(rt_builder_t *b, uint64_t pa, uint32_t perm)
{
    uint64_t l0_idx = (pa >> RT_L0_SHIFT) & RT_IDX_MASK;
    uint64_t l1_idx = (pa >> RT_L1_SHIFT) & RT_IDX_MASK;
    uint64_t l2_idx = (pa >> RT_L2_SHIFT) & RT_IDX_MASK;
    int have_pud, have_pmd;
    uint64_t pud_pa_local;
    uint64_t *pud_va_local;
    uint64_t pmd_pa;
    uint64_t *pmd_va;
    int rc;

    if (l0_idx >= 2u) return -ERANGE;
    if (l1_idx >= 512u) return -ERANGE;
    if (l2_idx >= 512u) return -ERANGE;

    have_pud = rt_find_pud(b, l0_idx, &pud_pa_local, &pud_va_local);
    if (!have_pud) {
        rc = rt_install_pud(b, l0_idx);
        if (rc != 0) return rc;
        rc = rt_find_pud(b, l0_idx, &pud_pa_local, &pud_va_local);
        if (rc == 0) return -EINVAL;
    }

    have_pmd = rt_find_pmd(b, l0_idx, l1_idx, &pmd_pa, &pmd_va);
    if (!have_pmd) {
        rc = rt_install_pmd(b, l0_idx, l1_idx);
        if (rc != 0) return rc;
        rc = rt_find_pmd(b, l0_idx, l1_idx, &pmd_pa, &pmd_va);
        if (rc == 0) return -EINVAL;
    }

    if (pmd_va[l2_idx] != 0u) {
        return -EINVAL;
    }
    pmd_va[l2_idx] = rt_encode_block_desc(pa, perm);
    return 0;
}

typedef struct {
    rt_builder_t *b;
    int err;
} rt_install_ctx_t;

static int rt_install_visit(void *ctx, uint64_t pa, uint32_t perm)
{
    rt_install_ctx_t *c = (rt_install_ctx_t *)ctx;
    int rc = rt_install_block(c->b, pa, perm);
    if (rc != 0) { c->err = rc; return 1; }
    return 0;
}

static void rt_zero_tree_out(struct aarch64_runtime_tree *out)
{
    uint8_t *p = (uint8_t *)out;
    size_t i;
    for (i = 0u; i < sizeof(*out); ++i) p[i] = 0u;
}

int aarch64_runtime_tree_build(const struct MEMORY_RANGE *ram, size_t count,
                               const struct aarch64_m1_arena *arena,
                               const struct aarch64_tree_ops *ops,
                               struct aarch64_runtime_tree *out)
{
    rt_builder_t b;
    rt_install_ctx_t ictx;
    uint64_t root_pa = 0;
    uint64_t *root_va = NULL;
    int err = 0;
    int rc;

    if (out != NULL) rt_zero_tree_out(out);
    if (ops == NULL || arena == NULL || out == NULL) return -EINVAL;
    if (ops->alloc == NULL || ops->resolve == NULL) return -EINVAL;
    if (count > MEMORY_RANGE_MAX) return -EINVAL;
    if (ram == NULL && count > 0u) return -EINVAL;
    if (arena->table_base_pa == 0u) return -EINVAL;
    if ((arena->table_base_pa & (RT_PAGE_4K - 1u)) != 0u) return -EINVAL;
    if (arena->table_pages == 0u) return -EINVAL;

    rc = rt_validate_ram(ram, count, &err);
    if (rc != 0) return err;

    {
        uint8_t *bp = (uint8_t *)&b;
        size_t i;
        for (i = 0u; i < sizeof(b); ++i) bp[i] = 0u;
    }
    b.ops = ops;
    b.pages_cap = arena->table_pages;

    rc = rt_alloc_one(&b, &root_pa, &root_va);
    if (rc != 0) return rc;
    b.root_va = root_va;

    ictx.b = &b;
    ictx.err = 0;
    rc = rt_walk_expected_blocks(ram, count, rt_install_visit, &ictx);
    if (rc != 0) return ictx.err != 0 ? ictx.err : -EINVAL;

    if (b.pages_issued > b.pages_cap) return -ENOMEM;

    out->root_pa = b.first_pa;
    out->table_base_pa = b.first_pa;
    out->table_used_end_pa = b.last_pa + RT_PAGE_4K;
    out->page_count = b.pages_issued;
    return 0;
}

/* ── Validation ───────────────────────────────────────────────
 *
 * The validator walks the tree and the expected set in lockstep in
 * PA-ascending order. Each step: extract the next (pa, desc) from the
 * tree iterator (block descriptors only) and the next (pa, perm) from
 * the expected iterator. Compare PAs and (recomputed) descriptors. If
 * either iterator ends early, error.
 *
 * The tree iterator uses a small explicit stack (max depth 3:
 * L0 → L1 → L2) and yields one block at a time. As it descends and
 * ascends, it accumulates bookkeeping for uniqueness / pool ownership
 * / SBZ-bit checks. The two uniqueness bitmaps (seen/children,
 * ~16 KiB) live in caller-provided storage (`struct
 * aarch64_runtime_tree_validate_buf`) so the validator's stack frame
 * fits on the aarch64 BSP boot stack (4 KiB). No large heap
 * allocation is required.
 */

#define RT_MAX_DEPTH 3u

typedef struct rt_level {
    uint64_t pa;
    uint64_t *va;
    uint64_t slot;
    int level;
    /* PA bucket base for this level: the smallest PA covered by the
     * table. For L0: the L0 entry's bucket base (0 for entry 0, 512 GiB
     * for entry 1, etc.). For PUD: l0_bucket + slot * 1 GiB (set when
     * we descend from L0). For PMD: same as the parent PUD. The PMD
     * uses this to recover the slot's PA from its slot index. */
    uint64_t bucket_base;
} rt_level_t;

typedef struct rt_tree_iter {
    const struct aarch64_tree_ops *ops;
    const struct aarch64_runtime_tree *tree;
    /* Caller-provided scratch: the intermediate-uniqueness bitmaps
     * live in `struct aarch64_runtime_tree_validate_buf` (a ~16 KiB
     * struct that must NOT be on the BSP boot stack). The iterator
     * itself is small enough to keep on the boot stack. */
    struct aarch64_runtime_tree_validate_buf *vbuf;
    rt_level_t stack[RT_MAX_DEPTH];
    int sp;
    size_t seen_count;
    size_t children_count;
    /* Currently yielded values. */
    uint64_t cur_pa;
    uint64_t cur_desc;
    int has_yield;
    int err;
} rt_tree_iter_t;

/* Record that we descended into a table page with PA `pa`. Returns
 * -EIO if it was already recorded (cycle) or if it's outside the
 * used pool (ownership). */
static int rt_iter_record_table(rt_tree_iter_t *it, uint64_t pa)
{
    size_t i;
    if (pa < it->tree->table_base_pa ||
        pa >= it->tree->table_used_end_pa) {
        return -EIO;
    }
    for (i = 0u; i < it->seen_count; ++i) {
        if (it->vbuf->seen[i] == pa) return -EIO;     /* cycle */
    }
    if (it->seen_count >=
        (sizeof(it->vbuf->seen) / sizeof(it->vbuf->seen[0])))
        return -EIO;
    it->vbuf->seen[it->seen_count++] = pa;
    return 0;
}

/* Record a child PA claimed by some intermediate. Returns -EIO if
 * the same PA was claimed twice (duplicate intermediate). */
static int rt_iter_record_child(rt_tree_iter_t *it, uint64_t pa)
{
    size_t i;
    if (pa < it->tree->table_base_pa ||
        pa >= it->tree->table_used_end_pa) {
        return -EIO;
    }
    for (i = 0u; i < it->children_count; ++i) {
        if (it->vbuf->children[i] == pa) return -EIO;     /* duplicate */
    }
    if (it->children_count >=
        (sizeof(it->vbuf->children) / sizeof(it->vbuf->children[0])))
        return -EIO;
    it->vbuf->children[it->children_count++] = pa;
    return 0;
}

static int rt_iter_push(rt_tree_iter_t *it, uint64_t pa, uint64_t *va,
                        int level, uint64_t bucket_base)
{
    int rc;
    if (it->sp >= (int)RT_MAX_DEPTH) return -EIO;
    rc = rt_iter_record_table(it, pa);
    if (rc != 0) return rc;
    it->stack[it->sp].pa = pa;
    it->stack[it->sp].va = va;
    it->stack[it->sp].slot = 0u;
    it->stack[it->sp].level = level;
    it->stack[it->sp].bucket_base = bucket_base;
    ++it->sp;
    return 0;
}

/* Initialize. The caller MUST have already zero-initialised vbuf's
 * bitmaps (the validate function does this on entry). */
static int rt_iter_init(rt_tree_iter_t *it,
                        const struct aarch64_tree_ops *ops,
                        const struct aarch64_runtime_tree *tree,
                        struct aarch64_runtime_tree_validate_buf *vbuf)
{
    uint64_t *root_va;

    {
        uint8_t *p = (uint8_t *)it;
        size_t i;
        for (i = 0u; i < sizeof(*it); ++i) p[i] = 0u;
    }
    it->ops = ops;
    it->tree = tree;
    it->vbuf = vbuf;

    if (tree->root_pa < tree->table_base_pa ||
        tree->root_pa >= tree->table_used_end_pa) {
        return -EIO;
    }

    root_va = ops->resolve(ops->ctx, tree->root_pa);
    if (root_va == NULL) return -EIO;

    /* L0 has no bucket base — its entries' bucket bases are computed
     * during descent (each L0[i] covers 512 GiB starting at i * 512 GiB). */
    return rt_iter_push(it, tree->root_pa, root_va, 0, 0);
}

/* Advance until the next yield (block descriptor) is ready. Returns
 * 1 if a yield is now ready (it->has_yield = 1), 0 if exhausted,
 * -1 on a contract failure. The caller consumes the yield via
 * rt_iter_take. */
static int rt_iter_advance(rt_tree_iter_t *it);

/* Read the top of the stack's current slot. Returns 0 if the slot is
 * valid (desc set), 1 if the top is past the end and we should pop. */
static int rt_iter_read_slot(rt_tree_iter_t *it, uint64_t *desc)
{
    rt_level_t *top;
    if (it->sp <= 0) return 1;
    top = &it->stack[it->sp - 1];
    if (top->slot >= RT_ENTRIES) return 1;
    *desc = top->va[top->slot];
    ++top->slot;
    return 0;
}

/* Pop the top of the explicit stack. */
static void rt_iter_pop(rt_tree_iter_t *it)
{
    if (it->sp <= 0) return;
    --it->sp;
}

static int rt_iter_advance(rt_tree_iter_t *it)
{
    while (it->sp > 0 && !it->has_yield && it->err == 0) {
        uint64_t desc = 0;
        int eos = rt_iter_read_slot(it, &desc);
        rt_level_t *top;
        uint64_t valid, is_table, child_pa, *child_va, out_pa, slot_pa;
        int rc;

        while (eos && it->sp > 0) {
            rt_iter_pop(it);
            if (it->sp <= 0) break;
            eos = rt_iter_read_slot(it, &desc);
        }
        if (it->sp <= 0) break;
        if (eos) continue;

        top = &it->stack[it->sp - 1];
        valid = desc & RT_DESC_VALID;
        is_table = desc & RT_DESC_TABLE;

        if (valid == 0u) continue;   /* V=0 — skip to next slot */

        if (is_table) {
            uint64_t bits_outside = desc & ~(RT_DESC_VALID |
                                              RT_DESC_TABLE |
                                              RT_DESC_L_PA_MASK);
            if (bits_outside != 0u) { it->err = -EIO; return -1; }
            if (top->level == 2) { it->err = -EIO; return -1; }
            child_pa = desc & RT_DESC_L_PA_MASK;
            child_va = it->ops->resolve(it->ops->ctx, child_pa);
            if (child_va == NULL) { it->err = -EIO; return -1; }
            rc = rt_iter_record_child(it, child_pa);
            if (rc != 0) { it->err = rc; return -1; }
            /* The slot we just read (top->slot - 1) is the parent
             * table's entry index. The child's bucket base is:
             *   parent's bucket_base + slot_idx * child_span
             * where child_span is 512 GiB for L0→PUD or 1 GiB for
             * PUD→PMD. */
            {
                uint64_t parent_slot = top->slot - 1u;
                uint64_t child_bucket;
                uint64_t child_span = (top->level == 0)
                    ? UINT64_C(0x8000000000)   /* 512 GiB */
                    : UINT64_C(0x40000000);    /* 1 GiB */
                child_bucket = top->bucket_base + parent_slot * child_span;
                rc = rt_iter_push(it, child_pa, child_va,
                                  top->level + 1, child_bucket);
            }
            if (rc != 0) { it->err = rc; return -1; }
            continue;
        }

        /* Block descriptor — only valid at L2. */
        if (top->level != 2) { it->err = -EIO; return -1; }

        /* The descriptor's PA field MUST match the slot's base PA
         * (so the validator rejects a block descriptor at the wrong
         * slot). The slot's PA = bucket_base + slot_idx * 2 MiB. */
        out_pa = desc & RT_DESC_L2_PA_MASK;
        slot_pa = top->bucket_base + ((top->slot - 1u) << RT_L2_SHIFT);
        if (out_pa != slot_pa) { it->err = -EIO; return -1; }

        it->cur_pa = slot_pa;
        it->cur_desc = desc;
        it->has_yield = 1;
        return 1;
    }

    if (it->has_yield) return 1;
    if (it->err != 0) return -1;
    return 0;
}

static int rt_iter_take(rt_tree_iter_t *it, uint64_t *out_pa,
                        uint64_t *out_desc)
{
    if (it->has_yield) {
        it->has_yield = 0;
        *out_pa = it->cur_pa;
        *out_desc = it->cur_desc;
        return 1;
    }
    {
        int rc = rt_iter_advance(it);
        if (rc < 0) return rc;
        if (rc == 0) return 0;
        it->has_yield = 0;
        *out_pa = it->cur_pa;
        *out_desc = it->cur_desc;
        return 1;
    }
}

/* ── Expected-set iterator (PA-ascending) ─────────────────────
 *
 * Three streams in PA order:
 *   - D: 16 entries at PA 0x08000000 + i*0x200000 (i = 0..15).
 *   - B: 1 entry at PA 0x40000000.
 *   - R: per ram range, PA-ascending.
 *
 * D finishes well before B starts (D's max = 0x09e00000 < B = 0x40000000).
 * B finishes before R starts (R's first PA ≥ 0x40200000 > B = 0x40000000).
 */
typedef struct rt_exp_iter {
    const struct MEMORY_RANGE *ram;
    size_t count;
    size_t d_idx;
    int d_done;
    int b_done;
    size_t r_idx;
    uint64_t r_pa;
    int r_done;
    uint64_t cur_pa;
    uint32_t cur_perm;
    int has_yield;
    int err;
} rt_exp_iter_t;

static void rt_exp_iter_init(rt_exp_iter_t *it,
                             const struct MEMORY_RANGE *ram, size_t count)
{
    {
        uint8_t *p = (uint8_t *)it;
        size_t i;
        for (i = 0u; i < sizeof(*it); ++i) p[i] = 0u;
    }
    it->ram = ram;
    it->count = count;
    /* D: 16 entries (size is constant for the spec). */
    it->d_done = 0;
    /* B: 1 entry. */
    it->b_done = 0;
    /* R: start at ram[0].phys_start if count > 0. */
    if (count > 0u && ram != NULL) {
        it->r_pa = ram[0].phys_start;
        it->r_done = 0;
    } else {
        it->r_done = 1;
    }
}

static int rt_exp_iter_advance(rt_exp_iter_t *it);

/* Pick the smallest available PA among the three streams and
 * advance its cursor. */
static int rt_exp_iter_advance(rt_exp_iter_t *it)
{
    uint64_t best_pa = UINT64_MAX;
    int source = -1;     /* 0 = D, 1 = B, 2 = R */
    uint32_t perm = 0;

    if (!it->d_done) {
        uint64_t p = RT_D_BASE + (uint64_t)it->d_idx * RT_PAGE_2M;
        if (p < best_pa) { best_pa = p; source = 0; perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE; }
    }
    if (!it->b_done) {
        if (RT_B_BASE < best_pa) { best_pa = RT_B_BASE; source = 1; perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_EXEC; }
    }
    if (!it->r_done) {
        if (it->r_pa < best_pa) { best_pa = it->r_pa; source = 2; perm = AARCH64_PT_KERNEL_RW; }
    }

    if (source < 0) return 0;   /* exhausted */

    it->cur_pa = best_pa;
    it->cur_perm = perm;

    switch (source) {
    case 0:
        ++it->d_idx;
        if ((it->d_idx * RT_PAGE_2M) >= (RT_D_END - RT_D_BASE)) {
            it->d_done = 1;
        }
        break;
    case 1:
        it->b_done = 1;
        break;
    case 2: {
        uint64_t end = it->ram[it->r_idx].phys_end;
        it->r_pa += RT_PAGE_2M;
        if (it->r_pa >= end) {
            ++it->r_idx;
            if (it->r_idx >= it->count) {
                it->r_done = 1;
            } else {
                it->r_pa = it->ram[it->r_idx].phys_start;
            }
        }
        break;
    }
    }

    it->has_yield = 1;
    return 1;
}

static int rt_exp_iter_take(rt_exp_iter_t *it, uint64_t *out_pa,
                            uint32_t *out_perm)
{
    if (it->has_yield) {
        it->has_yield = 0;
        *out_pa = it->cur_pa;
        *out_perm = it->cur_perm;
        return 1;
    }
    {
        int rc = rt_exp_iter_advance(it);
        if (rc < 0) return rc;
        if (rc == 0) return 0;
        it->has_yield = 0;
        *out_pa = it->cur_pa;
        *out_perm = it->cur_perm;
        return 1;
    }
}

int aarch64_runtime_tree_validate(const struct MEMORY_RANGE *ram, size_t count,
                                  const struct aarch64_m1_arena *arena,
                                  const struct aarch64_tree_ops *ops,
                                  struct aarch64_runtime_tree_validate_buf *vbuf,
                                  const struct aarch64_runtime_tree *tree)
{
    rt_tree_iter_t titer;
    rt_exp_iter_t eiter;
    int rc;
    int err2;

    if (ops == NULL || tree == NULL || vbuf == NULL) return -EINVAL;
    if (ops->resolve == NULL) return -EINVAL;
    if (tree->root_pa == 0u) return -EIO;
    if ((tree->root_pa & (RT_PAGE_4K - 1u)) != 0u) return -EIO;
    if (tree->table_base_pa > tree->table_used_end_pa) return -EIO;
    if ((tree->table_base_pa & (RT_PAGE_4K - 1u)) != 0u) return -EIO;
    if ((tree->table_used_end_pa & (RT_PAGE_4K - 1u)) != 0u) return -EIO;
    if (arena != NULL && tree->page_count > arena->table_pages) return -EIO;
    if ((tree->table_used_end_pa - tree->table_base_pa) !=
        (uint64_t)tree->page_count * RT_PAGE_4K) return -EIO;

    /* Zero the caller-provided scratch on entry so a stale vbuf from
     * a prior validate call does not leak state into the new run. */
    {
        uint8_t *p = (uint8_t *)vbuf;
        size_t i;
        for (i = 0u; i < sizeof(*vbuf); ++i) p[i] = 0u;
    }

    err2 = 0;
    (void)rt_validate_ram(ram, count, &err2);
    if (err2 != 0) return err2;

    rc = rt_iter_init(&titer, ops, tree, vbuf);
    if (rc != 0) return rc;

    rt_exp_iter_init(&eiter, ram, count);

    /* Step the two iterators in lockstep. */
    for (;;) {
        uint64_t t_pa, t_desc, e_pa;
        uint32_t e_perm;
        int t_ok, e_ok;

        t_ok = rt_iter_take(&titer, &t_pa, &t_desc);
        if (t_ok < 0) return titer.err != 0 ? titer.err : -EIO;
        e_ok = rt_exp_iter_take(&eiter, &e_pa, &e_perm);
        if (e_ok < 0) return -EIO;

        if (t_ok == 0 && e_ok == 0) break;
        if (t_ok == 0 || e_ok == 0) return -EIO;
        if (t_pa != e_pa) return -EIO;
        if (t_desc != rt_encode_block_desc(e_pa, e_perm)) return -EIO;
    }

    if (titer.err != 0) return titer.err;

    /* Uniqueness: every intermediate PA recorded by the tree
     * iterator must be unique (already enforced in rt_iter_record_table)
     * and the total count must match. The seen[] set is exactly the
     * tree's intermediates; page_count is the count. */
    if (titer.seen_count != tree->page_count) return -EIO;

    /* Every child PA must also be in the seen set (every claimed
     * child is actually visited). This catches an L0/L1/L2 with a
     * bogus child PA outside the tree. */
    {
        size_t i, j;
        for (i = 0u; i < titer.children_count; ++i) {
            int found = 0;
            for (j = 0u; j < titer.seen_count; ++j) {
                if (titer.vbuf->children[i] ==
                    titer.vbuf->seen[j]) { found = 1; break; }
            }
            if (!found) return -EIO;
        }
    }

    return 0;
}