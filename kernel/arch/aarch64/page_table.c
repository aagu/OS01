/* kernel/arch/aarch64/page_table.c
 *
 * AArch64 stage-1 translation-table primitive layer (see header for
 * the public contract and scope). Owns every descriptor bit value in
 * the tree; callers never construct descriptors by hand.
 *
 * The implementation walks the installed 4-level 48-bit regime
 * (T0SZ = T1SZ = 16, IPS = 40, MAIR[0] = Device, MAIR[1] = Normal
 * WBWA). Constants below were chosen to match the boot tables installed
 * by head.S — see head.S `write_tcr_mair_ttbr` and the
 * PT_ATTR_NORMAL / PT_ATTR_DEV .equ values. If MAIR_EL1 ever changes,
 * AARCH64_PT_ATTR_NORMAL / AARCH64_PT_ATTR_DEVICE here must change
 * with it; otherwise mappings silently use the wrong memory type but
 * still query correctly (the most dangerous failure mode).
 */

#include <stddef.h>
#include <stdint.h>

#include <kernel/arch/mmu.h>
#include <kernel/arch/aarch64/page_table.h>
#include <kernel/pmm.h>

/* ── Constants private to this TU ──────────────────────────────── */

/* Mask of low 40 bits — IPS=40 physical address range. The descriptor's
 * address field for any level uses bits [47:12]; with IPS=40 bits
 * [47:40] are RES0 so the effective output PA is bits [39:12]. */
#define AARCH64_PT_PA_MASK     UINT64_C(0xffffffffff000)   /* bits [39:12] = PA */
#define AARCH64_PT_PA_LIMIT    UINT64_C(0x10000000000)     /* 1 TiB */
#define AARCH64_PT_VA_LIMIT_LO UINT64_C(0x0001000000000000) /* 2^48 */
#define AARCH64_PT_VA_HI_BASE  UINT64_C(0xffff000000000000)

/* VA L0/L1/L2/L3 index mask (9 bits). */
#define AARCH64_PT_IDX_MASK    UINT64_C(0x1ff)
#define AARCH64_PT_L0_SHIFT    39
#define AARCH64_PT_L1_SHIFT    30
#define AARCH64_PT_L2_SHIFT    21
#define AARCH64_PT_L3_SHIFT    12

/* Descriptor field bits. */
#define AARCH64_PT_DESC_VALID  UINT64_C(0x001)            /* bit 0 */
#define AARCH64_PT_DESC_TABLE  UINT64_C(0x002)            /* bit 1: L0/L1/L2 table, L3 page */
#define AARCH64_PT_DESC_AF     UINT64_C(0x400)            /* bit 10 */
#define AARCH64_PT_DESC_SH_IS  UINT64_C(0x300)            /* bits [9:8] inner-shareable */
#define AARCH64_PT_DESC_SH_NS  UINT64_C(0x000)            /* bits [9:8] non-shareable */

/* AttrIndx: bits [4:2]. Matches the boot-table encoding in head.S:
 *   PT_ATTR_DEV    = 0<<2 (AttrIdx 0 = Device-nGnRnE in MAIR_EL1[7:0])
 *   PT_ATTR_NORMAL = 1<<2 (AttrIdx 1 = Normal WBWA in MAIR_EL1[15:8])
 * If MAIR_EL1 is rebuilt, these MUST be updated to track the new
 * AttrIdx slot assignments. */
#define AARCH64_PT_ATTR_NORMAL UINT64_C(0x008)
#define AARCH64_PT_ATTR_DEVICE UINT64_C(0x000)

/* Execute-never bits. UXN clears for an executable user mapping;
 * PXN clears for an executable kernel mapping. */
#define AARCH64_PT_DESC_PXN    UINT64_C(0x20000000000000)  /* bit 53 */
#define AARCH64_PT_DESC_UXN    UINT64_C(0x40000000000000)  /* bit 54 */

/* AP[2:1] encoding for stage 1 (ARM ARM D4-1506):
 *   00 = EL1 RW,  EL0 no access      (kernel RW)
 *   01 = EL1 RW,  EL0 RW             (user RW)
 *   10 = EL1 RO,  EL0 no access      (kernel RO)
 *   11 = EL1 RO,  EL0 RO             (user RO)
 * Bits [7:6] of the descriptor. */
#define AARCH64_PT_AP_MASK     UINT64_C(0x180)            /* bits [7:6] */
#define AARCH64_PT_AP_KERNEL_RW UINT64_C(0x000)
#define AARCH64_PT_AP_USER_RW   UINT64_C(0x040)
#define AARCH64_PT_AP_KERNEL_RO UINT64_C(0x100)
#define AARCH64_PT_AP_USER_RO   UINT64_C(0x140)

/* TTBR0_EL1 layout. The base address field is bits [47:12]; the
 * permitted non-base bits are the ASID (bits [63:48]) and the CnP bit
 * (bit 0). All other bits must read as zero on the live TTBR. */
#define AARCH64_TTBR_BASE_MASK       UINT64_C(0x000000fffffff000)
#define AARCH64_TTBR_ALLOWED_NONBASE (UINT64_C(0xffff000000000000) | \
                                      UINT64_C(1))
#define AARCH64_TTBR_ALLOWED_MASK    (AARCH64_TTBR_BASE_MASK | \
                                      AARCH64_TTBR_ALLOWED_NONBASE)

/* Permission word bit set. */
#define AARCH64_PT_PERM_ALL_BITS \
    (AARCH64_PT_KERNEL_RO | AARCH64_PT_KERNEL_RW | \
     AARCH64_PT_USER_RO   | AARCH64_PT_USER_RW   | \
     AARCH64_PT_EXEC      | AARCH64_PT_DEVICE)

/* ── Forward decls ──────────────────────────────────────────────── */

static int  root_valid(const uint64_t *root);
static int  is_active_root(const uint64_t *root);
static void tlb_invalidate_local(uint64_t va);
static int  decode_perm(uint64_t desc, uint32_t *perm_out);
static int  encode_perm(uint32_t perm, uint64_t *desc_out);
static void zero_page_via_root(uint64_t *root_hint, uint64_t pa);
static int  parent_pa(uint64_t desc, uint64_t *pa_out);
static int  walk_to_l3(uint64_t *root, uint64_t va, bool create,
                       uint64_t **pte_out, int *result_out);

/* ── Small helpers ──────────────────────────────────────────────── */

/* Zero one 4 KiB table page through the high-half direct map. The
 * volatile store prevents the optimizer from collapsing the loop. */
static void zero_page_via_root(uint64_t *root_hint, uint64_t pa)
{
    volatile uint64_t *cursor = (volatile uint64_t *)(pa + ARCH_PAGE_OFFSET);
    volatile uint64_t *end    = cursor + (PAGE_4K_SIZE / sizeof(uint64_t));
    (void)root_hint;
    while (cursor < end) {
        *cursor = 0;
        ++cursor;
    }
}

/* True iff `va` is a canonical 48-bit AArch64 VA accepted by the
 * installed T0SZ/T1SZ=16 regime. */
static bool va_canonical(uint64_t va)
{
    uint64_t hi = va & UINT64_C(0xffff000000000000);
    return hi == 0 || hi == UINT64_C(0xffff000000000000);
}

/* `root` validation per spec: 4 KiB aligned, in
 * [ARCH_PAGE_OFFSET, ARCH_PAGE_OFFSET + 1 TiB), no overflow past the
 * high boundary. Returns OK on success or EINVAL otherwise. */
static int root_valid(const uint64_t *root)
{
    if (root == NULL) return AARCH64_PT_EINVAL;
    uintptr_t r = (uintptr_t)root;
    if ((r & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    uintptr_t lo = (uintptr_t)ARCH_PAGE_OFFSET;
    uintptr_t hi = lo + (uintptr_t)AARCH64_PT_PA_LIMIT;
    if (r < lo) return AARCH64_PT_EINVAL;
    if (r >= hi) return AARCH64_PT_EINVAL;
    if (hi < lo) return AARCH64_PT_EINVAL; /* overflow guard */
    return AARCH64_PT_OK;
}

/* True iff the active TTBR0_EL1 (read via arch_get_page_table()) points
 * at the same root as `root`. ASID bits [63:48] and the CnP bit 0 are
 * permitted non-base bits; everything else must be zero. The extracted
 * base must be nonzero, 4 KiB-aligned, and < 1 TiB (IPS=40). */
static int is_active_root(const uint64_t *root)
{
    uint64_t raw = (uint64_t)(uintptr_t)arch_get_page_table();
    if ((raw & ~AARCH64_TTBR_ALLOWED_MASK) != 0) return 0;
    uint64_t ttbr_pa = raw & AARCH64_TTBR_BASE_MASK;
    if (ttbr_pa == 0) return 0;
    if ((ttbr_pa & (PAGE_4K_SIZE - 1)) != 0) return 0;
    if (ttbr_pa >= AARCH64_PT_PA_LIMIT) return 0;
    uintptr_t active_root = (uintptr_t)ttbr_pa + (uintptr_t)ARCH_PAGE_OFFSET;
    if ((uintptr_t)ARCH_PAGE_OFFSET + (uintptr_t)AARCH64_PT_PA_LIMIT
        < (uintptr_t)ARCH_PAGE_OFFSET) return 0;
    return active_root == (uintptr_t)root;
}

/* Local TLB invalidation for a single 4 KiB VA. Per ARM ARM the TLBI
 * operand is VA[47:12]; the inner-shareable dsb + isb pair is the
 * spec's required completion fence after an active-root publication. */
static void tlb_invalidate_local(uint64_t va)
{
    __asm__ __volatile__(
        "tlbi vae1, %0\n\t"
        "dsb ish\n\t"
        "isb"
        :: "r"(va >> AARCH64_PT_L3_SHIFT) : "memory");
}

/* Issue `dsb ishst` so subsequent descriptor stores are visible before
 * any later TLBI. Matches the spec's "store; dsb ishst; ..." ordering
 * for each parent publication and L3 write. */
static void dsb_ishst(void)
{
    __asm__ __volatile__("dsb ishst" ::: "memory");
}

/* ── Parent descriptor validation ─────────────────────────────── */

/* For a valid PUD/PMD table descriptor (V=1, bit1=1) extract its PA.
 * Returns EINVAL when the descriptor is invalid or its PA is not
 * 4 KiB-aligned and below 1 TiB; in that case `*pa_out` is not
 * written and the caller must NOT form `pa + ARCH_PAGE_OFFSET`. */
static int parent_pa(uint64_t desc, uint64_t *pa_out)
{
    if ((desc & AARCH64_PT_DESC_VALID) == 0) return AARCH64_PT_EINVAL;
    if ((desc & AARCH64_PT_DESC_TABLE) == 0) return AARCH64_PT_EINVAL;
    uint64_t pa = desc & AARCH64_PT_PA_MASK;
    if (pa == 0) return AARCH64_PT_EINVAL;
    if ((pa & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if (pa >= AARCH64_PT_PA_LIMIT) return AARCH64_PT_EINVAL;
    *pa_out = pa;
    return AARCH64_PT_OK;
}

/* ── Permission encoding/decoding ─────────────────────────────── */

/* Translate the public permission word into an AArch64 descriptor's
 * AP / SH / AttrIndx / XN bits. Validates the word first; returns
 * EINVAL for an unknown bit, an ambiguous kernel/user or RO/RW
 * selection, or the DEVICE | EXEC combination. */
static int encode_perm(uint32_t perm, uint64_t *desc_out)
{
    if ((perm & ~AARCH64_PT_PERM_ALL_BITS) != 0) return AARCH64_PT_EINVAL;

    uint32_t access = perm & (AARCH64_PT_KERNEL_RO | AARCH64_PT_KERNEL_RW |
                              AARCH64_PT_USER_RO   | AARCH64_PT_USER_RW);
    if (access == 0) return AARCH64_PT_EINVAL;
    /* Exactly one kernel/user, exactly one RO/RW. */
    bool has_kernel = (perm & (AARCH64_PT_KERNEL_RO | AARCH64_PT_KERNEL_RW)) != 0;
    bool has_user   = (perm & (AARCH64_PT_USER_RO   | AARCH64_PT_USER_RW))   != 0;
    if (has_kernel == has_user) return AARCH64_PT_EINVAL;
    bool is_rw = (perm & (AARCH64_PT_KERNEL_RW | AARCH64_PT_USER_RW)) != 0;
    bool is_ro = (perm & (AARCH64_PT_KERNEL_RO | AARCH64_PT_USER_RO)) != 0;
    if (is_rw == is_ro) return AARCH64_PT_EINVAL;

    if ((perm & AARCH64_PT_DEVICE) && (perm & AARCH64_PT_EXEC))
        return AARCH64_PT_EINVAL;

    bool is_kernel = has_kernel;
    bool is_exec   = (perm & AARCH64_PT_EXEC) != 0;
    bool is_device = (perm & AARCH64_PT_DEVICE) != 0;

    uint64_t ap;
    if (is_kernel) ap = is_rw ? AARCH64_PT_AP_KERNEL_RW : AARCH64_PT_AP_KERNEL_RO;
    else           ap = is_rw ? AARCH64_PT_AP_USER_RW   : AARCH64_PT_AP_USER_RO;

    uint64_t attr = is_device ? AARCH64_PT_ATTR_DEVICE : AARCH64_PT_ATTR_NORMAL;
    uint64_t sh   = is_device ? AARCH64_PT_DESC_SH_NS  : AARCH64_PT_DESC_SH_IS;

    /* Execute-never policy: device always sets both XN bits;
     * non-device + EXEC clears only the matching privilege's XN bit;
     * default (non-device, no EXEC) sets both. */
    uint64_t pxn = AARCH64_PT_DESC_PXN;
    uint64_t uxn = AARCH64_PT_DESC_UXN;
    if (!is_device && is_exec) {
        if (is_kernel) pxn = 0;
        else           uxn = 0;
    }

    *desc_out = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE |
                AARCH64_PT_DESC_AF | attr | sh | ap | pxn | uxn;
    return AARCH64_PT_OK;
}

/* Inverse of encode_perm. Returns EINVAL only for a descriptor that
 * cannot be parsed as a valid 4 KiB leaf (i.e. bit 0 or bit 1 is
 * clear). All other bits are decoded into the public permission word
 * regardless of their original class — the caller is expected to
 * have written only descriptors produced by encode_perm(). */
static int decode_perm(uint64_t desc, uint32_t *perm_out)
{
    if ((desc & AARCH64_PT_DESC_VALID) == 0) return AARCH64_PT_EINVAL;
    if ((desc & AARCH64_PT_DESC_TABLE) == 0) return AARCH64_PT_EINVAL;

    uint64_t attr = desc & (UINT64_C(0x7) << 2); /* bits [4:2] */
    uint64_t ap   = desc & AARCH64_PT_AP_MASK;

    uint32_t perm = 0;
    if (attr == AARCH64_PT_ATTR_DEVICE)
        perm |= AARCH64_PT_DEVICE;

    bool is_kernel = (ap & UINT64_C(0x80)) == 0;  /* AP bit 7 */
    bool is_ro     = (ap & UINT64_C(0x40)) != 0;  /* AP bit 6 */
    if (is_kernel) perm |= is_ro ? AARCH64_PT_KERNEL_RO : AARCH64_PT_KERNEL_RW;
    else           perm |= is_ro ? AARCH64_PT_USER_RO   : AARCH64_PT_USER_RW;

    /* Exec when the privilege-appropriate XN bit is clear. */
    bool exec = false;
    if (is_kernel) exec = (desc & AARCH64_PT_DESC_PXN) == 0;
    else           exec = (desc & AARCH64_PT_DESC_UXN) == 0;
    if (exec) perm |= AARCH64_PT_EXEC;

    *perm_out = perm;
    return AARCH64_PT_OK;
}

/* ── Walking ──────────────────────────────────────────────────── */

/* For the slot at `parent[index]`, ensure a valid table descriptor is
 * present and return its direct-mapped virtual pointer via `*child_out`.
 *
 *   - create == false, slot invalid (V=0)        → *rc_out = ENOENT
 *   - create == true,  slot invalid              → alloc + zero + link
 *                                                  (+ TLBI if active)
 *                                                  failures free the page
 *                                                  and propagate ENOMEM
 *                                                  or EINVAL
 *   - slot valid, V=1, bit1=0
 *        L0 → RES0 in 4-level regime → EINVAL (no deref)
 *        PUD/PMD → block descriptor → ECONFLICT (no modification)
 *   - slot valid table (V=1, bit1=1) → validate its PA, return child ptr
 *
 * Returns 0 on success and -1 on any failure (with `*rc_out` set).
 * On success `*child_out` is the next-level table's direct-map pointer. */
static int ensure_child_table(uint64_t *root, uint64_t *parent,
                              uint64_t index, uint64_t va, bool create,
                              bool is_l0, uint64_t **child_out, int *rc_out)
{
    uint64_t desc = parent[index];

    if ((desc & AARCH64_PT_DESC_VALID) == 0) {
        if (!create) { *rc_out = AARCH64_PT_ENOENT; return -1; }
        uint64_t pa = alloc_4k_page();
        if (pa == 0) { *rc_out = AARCH64_PT_ENOMEM; return -1; }
        zero_page_via_root(root, pa);
        dsb_ishst();
        uint64_t new_desc;
        int enc_rc = encode_perm(AARCH64_PT_KERNEL_RW, &new_desc);
        /* encode_perm on KERNEL_RW is unreachable-fail; guard anyway. */
        if (enc_rc != AARCH64_PT_OK) {
            free_4k_page(pa);
            *rc_out = enc_rc;
            return -1;
        }
        new_desc |= pa & AARCH64_PT_PA_MASK;
        parent[index] = new_desc;
        dsb_ishst();
        if (is_active_root(root)) tlb_invalidate_local(va);
        desc = new_desc;
    } else if ((desc & AARCH64_PT_DESC_TABLE) == 0) {
        *rc_out = is_l0 ? AARCH64_PT_EINVAL : AARCH64_PT_ECONFLICT;
        return -1;
    }

    uint64_t pa;
    int prc = parent_pa(desc, &pa);
    if (prc != AARCH64_PT_OK) { *rc_out = prc; return -1; }
    *child_out = (uint64_t *)(pa + ARCH_PAGE_OFFSET);
    *rc_out = AARCH64_PT_OK;
    return 0;
}

/* Walk L0 → L1 → L2 → L3 for `va` against `root`. With create == false
 * returns OK only when every level is present; otherwise sets
 * *result_out to ENOENT, EINVAL, or ECONFLICT and returns -1.
 *
 * With create == true, allocates missing intermediate tables via
 * alloc_4k_page(). A failure after allocation but before linking
 * releases the just-allocated page with free_4k_page(). */
static int walk_to_l3(uint64_t *root, uint64_t va, bool create,
                      uint64_t **pte_out, int *result_out)
{
    uint64_t l0 = (va >> AARCH64_PT_L0_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l1 = (va >> AARCH64_PT_L1_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l2 = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l3 = (va >> AARCH64_PT_L3_SHIFT) & AARCH64_PT_IDX_MASK;

    uint64_t *pud = NULL, *pmd = NULL, *pte = NULL;
    int rc;

    if (ensure_child_table(root, root, l0, va, create, true,
                           &pud, &rc) != 0) { *result_out = rc; return -1; }
    if (ensure_child_table(root, pud, l1, va, create, false,
                           &pmd, &rc) != 0) { *result_out = rc; return -1; }
    if (ensure_child_table(root, pmd, l2, va, create, false,
                           &pte, &rc) != 0) { *result_out = rc; return -1; }

    /* L3 leaf slot: V=0 with create == true is the normal "allocate
     * then store" case (the caller does the store); V=0 with
     * create == false means query/unmap misses the leaf. V=1 bit1=0
     * is RES0 at L3 (no block). V=1 bit1=1 is the leaf. */
    uint64_t leaf = pte[l3];
    if ((leaf & AARCH64_PT_DESC_VALID) != 0 &&
        (leaf & AARCH64_PT_DESC_TABLE) == 0) {
        *result_out = AARCH64_PT_EINVAL;
        return -1;
    }

    *pte_out = &pte[l3];
    *result_out = AARCH64_PT_OK;
    return 0;
}

/* ── Public operations ──────────────────────────────────────────── */

int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                      uint32_t perm)
{
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if ((pa & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if (pa >= AARCH64_PT_PA_LIMIT) return AARCH64_PT_EINVAL;

    uint64_t desc;
    rv = encode_perm(perm, &desc);
    if (rv != AARCH64_PT_OK) return rv;
    desc |= pa & AARCH64_PT_PA_MASK;

    uint64_t *pte = NULL;
    int wr = AARCH64_PT_OK;
    if (walk_to_l3(root, va, true, &pte, &wr) != 0) return wr;

    if ((*pte & AARCH64_PT_DESC_VALID) != 0) return AARCH64_PT_EEXIST;

    *pte = desc;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    return AARCH64_PT_OK;
}

int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out)
{
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;

    /* walk_to_l3 takes a non-const root; const-cast is safe because
     * create == false (no allocations, no descriptor stores). */
    uint64_t *r = (uint64_t *)root;
    uint64_t *pte = NULL;
    int wr = AARCH64_PT_OK;
    if (walk_to_l3(r, va, false, &pte, &wr) != 0) return wr;

    uint64_t desc = *pte;
    if ((desc & AARCH64_PT_DESC_VALID) == 0) return AARCH64_PT_ENOENT;

    uint32_t perm;
    rv = decode_perm(desc, &perm);
    if (rv != AARCH64_PT_OK) return rv;
    if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
    if (perm_out) *perm_out = perm;
    return AARCH64_PT_OK;
}

int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out)
{
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;

    uint64_t *pte = NULL;
    int wr = AARCH64_PT_OK;
    if (walk_to_l3(root, va, false, &pte, &wr) != 0) return wr;

    uint64_t desc = *pte;
    if ((desc & AARCH64_PT_DESC_VALID) == 0) return AARCH64_PT_ENOENT;
    /* Block descriptors at PUD/PMD are already handled by walk_to_l3
     * (ECONFLICT) before reaching here; a leaf is V=1, bit1=1. */
    if ((desc & AARCH64_PT_DESC_TABLE) == 0) return AARCH64_PT_EINVAL;

    uint32_t perm;
    rv = decode_perm(desc, &perm);
    if (rv != AARCH64_PT_OK) return rv;
    if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
    if (perm_out) *perm_out = perm;

    *pte = 0;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    return AARCH64_PT_OK;
}

bool aarch64_pt_range_accessible(const uint64_t *root, uint64_t va,
                                 uint64_t length, bool write, bool user)
{
    if (length == 0) return true;
    if (va + length < va) return false;       /* overflow */
    if (root == NULL) return false;
    uintptr_t r = (uintptr_t)root;
    if ((r & (PAGE_4K_SIZE - 1)) != 0) return false;
    if (r < (uintptr_t)ARCH_PAGE_OFFSET) return false;
    if (r >= (uintptr_t)ARCH_PAGE_OFFSET + (uintptr_t)AARCH64_PT_PA_LIMIT)
        return false;

    uint64_t end = va + length;
    uint64_t cursor = va & ~(uint64_t)(PAGE_4K_SIZE - 1);
    while (cursor < end) {
        uint64_t pa;
        uint32_t perm;
        int rc = aarch64_pt_query_4k(root, cursor, &pa, &perm);
        if (rc != AARCH64_PT_OK) return false;
        /* Blocks installed by boot code → not in this layer's domain. */
        if (rc == AARCH64_PT_ECONFLICT) return false;
        bool have_user = (perm & AARCH64_PT_USER_RO) != 0 ||
                         (perm & AARCH64_PT_USER_RW) != 0;
        bool have_rw   = (perm & AARCH64_PT_KERNEL_RW) != 0 ||
                         (perm & AARCH64_PT_USER_RW)   != 0;
        if (user && !have_user) return false;
        if (write && !have_rw) return false;
        cursor += PAGE_4K_SIZE;
    }
    return true;
}