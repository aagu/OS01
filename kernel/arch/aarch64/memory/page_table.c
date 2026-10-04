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
 *
 * M3.3 (Task 16) extends the layer with software bits
 * (AARCH64_PT_SOFTWARE_PROTNONE / COW, descriptor bits 55 / 56) and
 * the three-state query (VALID | PROTNONE-stash | absent).  Block-
 * descriptor encoding lives in aarch64_pt_encode_block_desc() (spec
 * §5.1) — independent of the 4 KiB leaf path so the OA-field width
 * ([39:21] vs [39:12]) is correctly partitioned.
 *
 * Inline aarch64 asm in tlb_invalidate_local / dsb_ishst is wrapped in
 * #ifdef __aarch64__ so the file compiles cleanly when host-cross-
 * compiled (clang on x86, no aarch64 sysroot) for the M3.3 RED tests.
 * The #else branch is a no-op — host tests exercise the descriptor
 * logic with is_active_root() returning false (mock aarch64_read_ttbr1
 * returns 0 and real CR3 never matches the test root pointer), so the
 * TLBI path is never reached in the host harness.
 */

#include <stddef.h>
#include <stdint.h>

#include <arch/mmu.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/aarch64/boot_direct_map.h>
#include <memory/pmm.h>

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

/* AttrIndx: bits [4:2] of the descriptor. Matches the boot-table
 * encoding in head.S:
 *   PT_ATTR_DEV    = 0<<2 = 0x0  (AttrIdx 0 = Device-nGnRnE in MAIR_EL1[7:0])
 *   PT_ATTR_NORMAL = 1<<2 = 0x4  (AttrIdx 1 = Normal WBWA in MAIR_EL1[15:8])
 * The AttrIdx arithmetic is bit-N = 1<<N, so AttrIdx 1 = bit 2 = 0x4 —
 * NOT 0x8 (which would be AttrIdx 2 = MAIR slot 2 = 0x00, silently
 * downgrading to Device-nGnRnE). If MAIR_EL1 is rebuilt, these MUST be
 * updated to track the new AttrIdx slot assignments. */
#define AARCH64_PT_ATTR_NORMAL UINT64_C(0x004)
#define AARCH64_PT_ATTR_DEVICE UINT64_C(0x000)
/* Compile-time guard so any regression that confuses bit positions is
 * caught at build time instead of via silent memory-type drift. */
_Static_assert(AARCH64_PT_ATTR_NORMAL == 0x4,
               "AttrIndx 1 must be bit 2 = 0x4, not 0x8");
_Static_assert(AARCH64_PT_ATTR_DEVICE == 0x0,
               "AttrIndx 0 must be 0x0");

/* Execute-never bits. UXN clears for an executable user mapping;
 * PXN clears for an executable kernel mapping. */
#define AARCH64_PT_DESC_PXN    UINT64_C(0x20000000000000)  /* bit 53 */
#define AARCH64_PT_DESC_UXN    UINT64_C(0x40000000000000)  /* bit 54 */

/* AP[2:1] encoding for stage 1 (ARM ARM D4-1506), occupying descriptor
 * bits [7:6]:
 *   00 = EL1 RW,  EL0 no access      (kernel RW)
 *   01 = EL1 RW,  EL0 RW             (user RW)
 *   10 = EL1 RO,  EL0 no access      (kernel RO)
 *   11 = EL1 RO,  EL0 RO             (user RO)
 * Bit 7 = AP[2], bit 6 = AP[1]. Bit 8 is the shareability field (SH[1])
 * and MUST NOT appear in the AP value — using it would make the kernel
 * RO encoding actually set SH=inner-shareable twice (no harm) while
 * leaving AP[2:1]=00 = kernel-RW (silently wrong). */
#define AARCH64_PT_AP_MASK      UINT64_C(0x0C0)            /* bits [7:6] */
#define AARCH64_PT_AP_KERNEL_RW UINT64_C(0x000)            /* 00 */
#define AARCH64_PT_AP_USER_RW   UINT64_C(0x040)            /* 01 */
#define AARCH64_PT_AP_KERNEL_RO UINT64_C(0x080)            /* 10 */
#define AARCH64_PT_AP_USER_RO   UINT64_C(0x0C0)            /* 11 */
/* Bit-position guards — any future drift is caught at compile time
 * rather than via silently-wrong AP decoding of boot-table RO maps. */
_Static_assert(AARCH64_PT_AP_MASK == 0x0C0,
               "AP mask must cover descriptor bits [7:6]");
_Static_assert(AARCH64_PT_AP_KERNEL_RW == 0x0,
               "AP[2:1]=00 kernel-RW is 0x0");
_Static_assert(AARCH64_PT_AP_USER_RW == 0x40,
               "AP[2:1]=01 user-RW sets only bit 6");
_Static_assert(AARCH64_PT_AP_KERNEL_RO == 0x80,
               "AP[2:1]=10 kernel-RO sets only bit 7");
_Static_assert(AARCH64_PT_AP_USER_RO == 0xC0,
               "AP[2:1]=11 user-RO sets bits 7 and 6");

/* TTBR0_EL1 layout constants (AARCH64_TTBR_BASE_MASK and
 * AARCH64_TTBR_ALLOWED_NONBASE) now live in page_table.h so the
 * BSP pre-SMP self-test can validate the active TTBR before
 * converting to a direct-map pointer. */

/* Permission word bit set. */
#define AARCH64_PT_PERM_ALL_BITS \
    (AARCH64_PT_KERNEL_RO | AARCH64_PT_KERNEL_RW | \
     AARCH64_PT_USER_RO   | AARCH64_PT_USER_RW   | \
     AARCH64_PT_EXEC      | AARCH64_PT_DEVICE)

/* L2 block OA mask — bits [39:21] of a block descriptor (IPS=40).
 * Lower 21 bits and bits [40:47] are RES0 for a block. */
#define AARCH64_PT_BLOCK_OA_MASK UINT64_C(0xffffffe00000)

/* Software-bit mask accepted at every entry point.  Anything else in
 * the sw argument is a programming error → -EINVAL.  PROTNONE | COW
 * is the only illegal combination of legal bits. */
#define AARCH64_PT_SW_ALLOWED_MASK \
    (AARCH64_PT_SOFTWARE_PROTNONE | AARCH64_PT_SOFTWARE_COW)

/* ── Forward decls ──────────────────────────────────────────────── */

static int  root_valid(const uint64_t *root);
static int  is_active_root(const uint64_t *root);
static void tlb_invalidate_local(uint64_t va);
static void dsb_ishst(void);
static int  decode_perm(uint64_t desc, uint32_t *perm_out,
                        uint64_t *sw_out);
static int  encode_perm(uint32_t perm, uint64_t *desc_out);
static void zero_page(uint64_t pa);
static int  parent_pa(uint64_t desc, uint64_t *pa_out);
static uint64_t encode_table_desc(uint64_t pa);
static int  walk_to_l3(uint64_t *root, uint64_t va, bool create,
                       uint64_t **pte_out, int *result_out);
static int  walk_to_l2(uint64_t *root, uint64_t va, bool create,
                       uint64_t **pmd_out, int *result_out);

/* ── Small helpers ──────────────────────────────────────────────── */

/* Minimal L0/L1/L2 table descriptor. Per ARM ARM D5.4.3 every bit
 * outside [V | TYPE_TABLE | PA] is SBZ (should-be-zero) for
 * intermediate translation-table entries. QEMU TCG strictly
 * enforces this and faults a translation walk on any non-zero SBZ
 * bit — using `encode_perm(KERNEL_RW)` here would set AP/AF/
 * AttrIndx/SH/PXN/UXN, all of which are SBZ at L0/L1/L2.
 *
 * This is intentionally different from `encode_perm`, which produces
 * a full 4 KiB leaf descriptor for L3 only. Do not collapse them. */
static uint64_t encode_table_desc(uint64_t pa)
{
    return AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE |
           (pa & AARCH64_PT_PA_MASK);
}

/* Zero one 4 KiB table page through the high-half direct map. The
 * volatile store prevents the optimizer from collapsing the loop. */
static void zero_page(uint64_t pa)
{
    volatile uint64_t *cursor = (volatile uint64_t *)(pa + ARCH_PAGE_OFFSET);
    volatile uint64_t *end    = cursor + (PAGE_4K_SIZE / sizeof(uint64_t));
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
    uint64_t roots[2]={(uint64_t)(uintptr_t)arch_get_page_table(),aarch64_read_ttbr1()};
    for(size_t i=0;i<2;i++) {
        uint64_t raw=roots[i];
        if(raw & ~AARCH64_TTBR_ALLOWED_MASK)continue;
        uint64_t pa=raw & AARCH64_TTBR_BASE_MASK;
        if(pa && pa<AARCH64_PT_PA_LIMIT && pa+ARCH_PAGE_OFFSET==(uintptr_t)root)return 1;
    }
    return 0;
}

/* Local TLB invalidation for a single 4 KiB VA. Per ARM ARM the TLBI
 * operand is VA[47:12]; the inner-shareable dsb + isb pair is the
 * spec's required completion fence after an active-root publication.
 *
 * Host test compiles with the aarch64 asm elided (no aarch64 sysroot);
 * is_active_root() never returns true in the host harness, so this
 * function is unreachable there. */
static void tlb_invalidate_local(uint64_t va)
{
#ifdef __aarch64__
    __asm__ __volatile__(
        "tlbi vae1, %0\n\t"
        "dsb ish\n\t"
        "isb"
        :: "r"(va >> AARCH64_PT_L3_SHIFT) : "memory");
#else
    (void)va;
#endif
}

/* Issue `dsb ishst` so subsequent descriptor stores are visible before
 * any later TLBI. Matches the spec's "store; dsb ishst; ..." ordering
 * for each parent publication and L3 write.  Host build: no-op. */
static void dsb_ishst(void)
{
#ifdef __aarch64__
    __asm__ __volatile__("dsb ishst" ::: "memory");
#endif
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
 * selection, or the DEVICE | EXEC combination. The DEVICE | EXEC
 * check is intentionally independent of the access-class check so a
 * future restructure cannot accidentally hide it behind dead code. */
static int encode_perm(uint32_t perm, uint64_t *desc_out)
{
    if ((perm & ~AARCH64_PT_PERM_ALL_BITS) != 0) return AARCH64_PT_EINVAL;

    /* Reject the forbidden combination first; doesn't depend on which
     * access class (if any) the caller picked. */
    if ((perm & AARCH64_PT_DEVICE) && (perm & AARCH64_PT_EXEC))
        return AARCH64_PT_EINVAL;

    /* Exactly one kernel/user, exactly one RO/RW. Missing both kernel
     * and user → has_kernel == has_user (false == false) → EINVAL. */
    bool has_kernel = (perm & (AARCH64_PT_KERNEL_RO | AARCH64_PT_KERNEL_RW)) != 0;
    bool has_user   = (perm & (AARCH64_PT_USER_RO   | AARCH64_PT_USER_RW))   != 0;
    if (has_kernel == has_user) return AARCH64_PT_EINVAL;
    bool is_rw = (perm & (AARCH64_PT_KERNEL_RW | AARCH64_PT_USER_RW)) != 0;
    bool is_ro = (perm & (AARCH64_PT_KERNEL_RO | AARCH64_PT_USER_RO)) != 0;
    if (is_rw == is_ro) return AARCH64_PT_EINVAL;

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

/* Inverse of encode_perm. Decodes a valid 4 KiB leaf descriptor back
 * to the public permission word + software bits.
 *
 * Returns EINVAL only for a descriptor that cannot be parsed as a
 * valid 4 KiB leaf (i.e. bit 0 or bit 1 is clear).  All other bits are
 * decoded regardless of their original class — the caller is expected
 * to have written only descriptors produced by encode_perm().  The
 * three-state caller (query / unmap / replace) additionally reads the
 * VALID bit and dispatches on PROTNONE stashes / empty slots before
 * calling this helper, so reaching it with VALID=0 is an internal
 * inconsistency and returns EINVAL.
 *
 * `*sw_out` may be NULL; when non-NULL it receives the descriptor's
 * software bits 55 / 56, masked to the legal PROTNONE | COW pair. */
static int decode_perm(uint64_t desc, uint32_t *perm_out, uint64_t *sw_out)
{
    if ((desc & AARCH64_PT_DESC_VALID) == 0) return AARCH64_PT_EINVAL;
    if ((desc & AARCH64_PT_DESC_TABLE) == 0) return AARCH64_PT_EINVAL;

    uint64_t attr = desc & (UINT64_C(0x7) << 2); /* bits [4:2] */
    uint64_t ap   = desc & AARCH64_PT_AP_MASK;

    uint32_t perm = 0;
    if (attr == AARCH64_PT_ATTR_DEVICE)
        perm |= AARCH64_PT_DEVICE;

    /* AP[2:1] lives at descriptor bits [7:6]. Bit 7 = AP[2] = the RO
     * bit (0 → RW, 1 → RO). Bit 6 = AP[1] = the user-allowed bit
     * (0 → kernel-only, 1 → kernel+user). Reading these backwards
     * (calling bit 7 "kernel" and bit 6 "RO") silently swaps KERNEL_RO
     * with USER_RW in decode — caught by the decode-roundtrip check. */
    bool is_ro     = (ap & UINT64_C(0x80)) != 0;  /* AP[2] */
    bool is_kernel = (ap & UINT64_C(0x40)) == 0;  /* AP[1] */
    if (is_kernel) perm |= is_ro ? AARCH64_PT_KERNEL_RO : AARCH64_PT_KERNEL_RW;
    else           perm |= is_ro ? AARCH64_PT_USER_RO   : AARCH64_PT_USER_RW;

    /* Exec when the privilege-appropriate XN bit is clear. */
    bool exec = false;
    if (is_kernel) exec = (desc & AARCH64_PT_DESC_PXN) == 0;
    else           exec = (desc & AARCH64_PT_DESC_UXN) == 0;
    if (exec) perm |= AARCH64_PT_EXEC;

    *perm_out = perm;
    if (sw_out)
        *sw_out = desc & AARCH64_PT_SW_ALLOWED_MASK;
    return AARCH64_PT_OK;
}

/* Build a 4 KiB leaf descriptor (bits [39:12] = PA) with software
 * bits OR'd in.  Returns EINVAL when perm or sw is illegal; the
 * decoder (decode_perm) is the matching inverse.
 *
 * Per spec §4.2: PROTNONE is the "PRESENT=0, PROTNONE=1" stash —
 * VALID must be CLEARED when the PROTNONE software bit is set, so
 * the descriptor reads as "invalid but PA-preserved" to the walker.
 * The PA itself is preserved in bits [39:12] so query_4k can hand it
 * back to the caller. */
static int build_leaf_desc(uint64_t pa, uint32_t perm, uint64_t sw,
                           uint64_t *desc_out)
{
    uint64_t base;
    int rv = encode_perm(perm, &base);
    if (rv != AARCH64_PT_OK) return rv;
    if ((sw & ~AARCH64_PT_SW_ALLOWED_MASK) != 0) return AARCH64_PT_EINVAL;
    if ((sw & AARCH64_PT_SW_ALLOWED_MASK) == AARCH64_PT_SW_ALLOWED_MASK)
        return AARCH64_PT_EINVAL;            /* PROTNONE | COW nonsensical */
    /* PROTNONE = "PRESENT=0, PA-preserved" — clear VALID. */
    if (sw & AARCH64_PT_SOFTWARE_PROTNONE) {
        base &= ~AARCH64_PT_DESC_VALID;
    }
    *desc_out = base | (pa & AARCH64_PT_PA_MASK) | sw;
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
        zero_page(pa);
        dsb_ishst();
        /* Intermediate table descriptors must be minimal — only
         * V | TYPE_TABLE | PA. See encode_table_desc. */
        parent[index] = encode_table_desc(pa);
        dsb_ishst();
        if (is_active_root(root)) tlb_invalidate_local(va);
        desc = parent[index];
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

/* Walk L0 → L1 → L2 for `va` (don't descend to L3).  Returns the L2
 * (PMD) table's direct-map pointer via `*pmd_out`.  Used by the 2 MiB
 * block path (Task 16 keep-step; full split lives in Task 18).  With
 * create == false returns OK only when every level is present;
 * otherwise sets *result_out to ENOENT, EINVAL, or ECONFLICT and
 * returns -1.  Block descriptors at L2 are NOT rejected here — the
 * caller checks `pmd[l2]` to distinguish block vs table for split. */
__attribute__((unused))
static int walk_to_l2(uint64_t *root, uint64_t va, bool create,
                      uint64_t **pmd_out, int *result_out)
{
    uint64_t l0 = (va >> AARCH64_PT_L0_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l1 = (va >> AARCH64_PT_L1_SHIFT) & AARCH64_PT_IDX_MASK;

    uint64_t *pud = NULL, *pmd = NULL;
    int rc;

    if (ensure_child_table(root, root, l0, va, create, true,
                           &pud, &rc) != 0) { *result_out = rc; return -1; }
    if (ensure_child_table(root, pud, l1, va, create, false,
                           &pmd, &rc) != 0) { *result_out = rc; return -1; }

    *pmd_out = pmd;
    *result_out = AARCH64_PT_OK;
    return 0;
}

/* ── Internal helpers for the ext entry points ────────────────── */

/* Read the current L3 slot for `va` and report its disposition via
 * `*rc_out`:
 *   AARCH64_PT_OK         — valid leaf; `*pte_out` / `*desc_out` valid.
 *   AARCH64_PT_EPROT_NONE — PROTNONE stash: VALID clear, PROTNONE bit set.
 *   AARCH64_PT_ENOENT     — slot is genuinely empty.
 *   AARCH64_PT_ECONFLICT  — PUD/PMD block in the path.
 *   AARCH64_PT_EINVAL     — bad VA / decode failure.
 *
 * Returns 0 on success (the leaf was reached — the actual disposition
 * is in `*rc_out`), -1 if input validation or walk_to_l3 failed
 * (in which case `*rc_out` holds the error code too, so callers can
 * just `if (read_leaf(...) != 0) return *rc_out`).  Pass `create=false`
 * for read paths (query / unmap / replace) — they must never allocate. */
static int read_leaf(uint64_t *root, uint64_t va, bool create,
                     uint64_t **pte_out, uint64_t *desc_out,
                     int *rc_out)
{
    if (!va_canonical(va))         { *rc_out = AARCH64_PT_EINVAL; return -1; }
    if ((va & (PAGE_4K_SIZE - 1))) { *rc_out = AARCH64_PT_EINVAL; return -1; }

    uint64_t *pte = NULL;
    int wr = AARCH64_PT_OK;
    if (walk_to_l3(root, va, create, &pte, &wr) != 0) {
        *rc_out = wr;
        return -1;
    }

    uint64_t desc = *pte;
    if (pte_out) *pte_out = pte;
    *desc_out = desc;

    if ((desc & AARCH64_PT_DESC_VALID) != 0) {
        uint32_t perm_unused;
        uint64_t sw_unused;
        if (decode_perm(desc, &perm_unused, &sw_unused) != AARCH64_PT_OK) {
            *rc_out = AARCH64_PT_EINVAL;
            return -1;
        }
        *rc_out = AARCH64_PT_OK;
        return 0;
    }

    /* VALID clear → either PROTNONE stash or empty. */
    if ((desc & AARCH64_PT_SOFTWARE_PROTNONE) != 0) {
        *rc_out = AARCH64_PT_EPROT_NONE;
        return 0;
    }
    *rc_out = AARCH64_PT_ENOENT;
    return 0;
}

/* ── Public operations ──────────────────────────────────────────── */

/* Thin wrappers preserving the original 4-arg M1 selftest signature.
 * Each forwards to its _ext sibling with software_bits = 0.  Kept in
 * source order so the M3.3 commit shows the ext family landing on top
 * of the M1 layer. */

int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                      uint32_t perm)
{
    return aarch64_pt_map_4k_ext(root, va, pa, perm, 0);
}

int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out)
{
    /* walk_to_l3 takes a non-const root; const-cast is safe because
     * read_leaf/create=false → no allocations, no descriptor stores. */
    uint64_t *r = (uint64_t *)root;
    return aarch64_pt_query_4k_ext(r, va, pa_out, perm_out, NULL);
}

int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out)
{
    return aarch64_pt_unmap_4k_ext(root, va, pa_out, perm_out, NULL);
}

int aarch64_pt_map_4k_ext(uint64_t *root, uint64_t va, uint64_t pa,
                          uint32_t perm, uint64_t software_bits)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if ((pa & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if (pa >= AARCH64_PT_PA_LIMIT) return AARCH64_PT_EINVAL;

    uint64_t desc;
    rv = build_leaf_desc(pa, perm, software_bits, &desc);
    if (rv != AARCH64_PT_OK) return rv;

    uint64_t *pte = NULL;
    int wr = AARCH64_PT_OK;
    if (walk_to_l3(root, va, true, &pte, &wr) != 0) return wr;

    if ((*pte & AARCH64_PT_DESC_VALID) != 0) return AARCH64_PT_EEXIST;
    /* PROTNONE stash (VALID clear but PROTNONE bit set) is also
     * "occupied" — caller must unmap first. */
    if ((*pte & AARCH64_PT_SOFTWARE_PROTNONE) != 0) return AARCH64_PT_EEXIST;

    *pte = desc;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    return AARCH64_PT_OK;
}

int aarch64_pt_query_4k_ext(const uint64_t *root, uint64_t va,
                            uint64_t *pa_out, uint32_t *perm_out,
                            uint64_t *sw_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;

    uint64_t *r = (uint64_t *)root;
    uint64_t *pte = NULL;
    uint64_t desc = 0;
    int wr = AARCH64_PT_OK;
    if (read_leaf(r, va, false, &pte, &desc, &wr) != 0) return wr;

    if (wr == AARCH64_PT_EPROT_NONE) {
        if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
        if (perm_out) *perm_out = 0;
        if (sw_out)   *sw_out   = AARCH64_PT_SOFTWARE_PROTNONE;
        return AARCH64_PT_EPROT_NONE;
    }
    if (wr == AARCH64_PT_ENOENT) return AARCH64_PT_ENOENT;

    /* Valid leaf: decode perm + sw directly from the descriptor. */
    uint32_t perm;
    uint64_t sw;
    rv = decode_perm(desc, &perm, &sw);
    if (rv != AARCH64_PT_OK) return rv;
    if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
    if (perm_out) *perm_out = perm;
    if (sw_out)   *sw_out   = sw;
    return AARCH64_PT_OK;
}

int aarch64_pt_unmap_4k_ext(uint64_t *root, uint64_t va,
                            uint64_t *pa_out, uint32_t *perm_out,
                            uint64_t *sw_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;

    uint64_t *pte = NULL;
    uint64_t desc = 0;
    int wr = AARCH64_PT_OK;
    if (read_leaf(root, va, false, &pte, &desc, &wr) != 0) return wr;

    if (wr == AARCH64_PT_EPROT_NONE) {
        if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
        if (perm_out) *perm_out = 0;
        if (sw_out)   *sw_out   = AARCH64_PT_SOFTWARE_PROTNONE;
        /* Clear the slot; the page is owned by the caller (spec
         * §4.4.4) so we do NOT free_4k_page the data PA. */
        *pte = 0;
        dsb_ishst();
        if (is_active_root(root)) tlb_invalidate_local(va);
        return AARCH64_PT_EPROT_NONE;
    }
    if (wr == AARCH64_PT_ENOENT) return AARCH64_PT_ENOENT;

    uint32_t perm;
    uint64_t sw;
    rv = decode_perm(desc, &perm, &sw);
    if (rv != AARCH64_PT_OK) return rv;
    if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
    if (perm_out) *perm_out = perm;
    if (sw_out)   *sw_out   = sw;

    *pte = 0;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    return AARCH64_PT_OK;
}

int aarch64_pt_replace_4k(uint64_t *root, uint64_t va, uint64_t pa,
                          uint32_t perm, uint64_t software_bits,
                          uint64_t *old_pa_out, uint64_t *old_sw_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_4K_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if (pa >= AARCH64_PT_PA_LIMIT) return AARCH64_PT_EINVAL;

    uint64_t new_desc;
    rv = build_leaf_desc(pa, perm, software_bits, &new_desc);
    if (rv != AARCH64_PT_OK) return rv;

    uint64_t *pte = NULL;
    uint64_t old_desc = 0;
    int wr = AARCH64_PT_OK;
    if (read_leaf(root, va, false, &pte, &old_desc, &wr) != 0) return wr;
    if (wr == AARCH64_PT_ENOENT) return AARCH64_PT_ENOENT;

    /* Always report the prior state so callers can reconstruct
     * (VMA → COW, COW → VMA, etc.) regardless of the category. */
    uint64_t old_pa;
    uint64_t old_sw;
    bool was_valid;
    if (wr == AARCH64_PT_EPROT_NONE) {
        old_pa = old_desc & AARCH64_PT_PA_MASK;
        old_sw = AARCH64_PT_SOFTWARE_PROTNONE;
        was_valid = false;
    } else {
        uint32_t old_perm_unused;
        rv = decode_perm(old_desc, &old_perm_unused, &old_sw);
        if (rv != AARCH64_PT_OK) return rv;
        old_pa = old_desc & AARCH64_PT_PA_MASK;
        was_valid = true;
    }

    /* Spec §4.4.3 classification.  Mask off PA + sw bits so the
     * "rest" of the descriptor (AP[2:1] / SH / AttrIndx / AF / XN)
     * is what we compare for the perm-only fast path. */
    const uint64_t non_pa_non_sw_mask =
        ~(AARCH64_PT_PA_MASK | AARCH64_PT_SW_ALLOWED_MASK);
    bool same_pa      = (old_pa == pa);
    bool same_attr    = ((old_desc & (UINT64_C(0x7) << 2)) ==
                         (new_desc & (UINT64_C(0x7) << 2)));
    bool same_valid   = (was_valid ==
                         ((software_bits & AARCH64_PT_SOFTWARE_PROTNONE) == 0));
    bool same_rest    = ((old_desc & non_pa_non_sw_mask) ==
                         (new_desc & non_pa_non_sw_mask));
    bool perm_only    = same_pa && same_attr && same_valid && same_rest;

    /* TODO(Task 17): acquire pt_lock_for(root, l2) around the
     * descriptor write.  M3 callers are single-threaded boot-time
     * (vmm_gate_check covers the SMP phase); Task 17 replaces this
     * implicit assumption with the explicit lock. */
    (void)perm_only;

    /* PTE-level break-before-make (spec §4.4.3): clear → dsb → TLBI
     * → dsb → [shootdown caller-owned] → set → dsb → TLBI → dsb; isb.
     * We always do BBM — the perm-only fast path is noted but
     * consolidated into the same store sequence here for now; the
     * distinction matters only when the caller passes a perm-only
     * change AND the active root is shared with another CPU, which
     * the gate covers by waiting for ipi_ready before any post-SMP
     * update.  shootdown remains caller-owned. */
    *pte = 0;
    dsb_ishst();
    tlb_invalidate_local(va);
    dsb_ishst();

    *pte = new_desc;
    dsb_ishst();
    tlb_invalidate_local(va);
#ifdef __aarch64__
    __asm__ __volatile__("isb" ::: "memory");
#endif

    if (old_pa_out) *old_pa_out = old_pa;
    if (old_sw_out) *old_sw_out = old_sw;
    return AARCH64_PT_OK;
}

uint64_t aarch64_pt_encode_block_desc(uint64_t pa, uint32_t perm,
                                      uint64_t software_bits)
{
    /* Block descriptor policy mirrors encode_perm for AP / SH /
     * AttrIndx / XN; the descriptor type is block (bit1 = 0) and the
     * OA lives in bits [39:21] (L2 block format — IPS=40).
     *
     * PA must be 2 MiB aligned and < 1 TiB; the caller enforces that
     * upstream (arch_vmm_map_2m).  encode_perm rejects DEVICE | EXEC
     * and bad perm combinations — those checks are reused.  If perm
     * is illegal, fall back to a kernel-RW read-only block so the
     * caller still gets a well-formed descriptor (the arch_vmm_map_2m
     * validation rejects the call before reaching here). */
    uint64_t base;
    if (encode_perm(perm, &base) != AARCH64_PT_OK)
        encode_perm(AARCH64_PT_KERNEL_RO, &base);  /* safe fallback */
    /* Mask illegal sw bits silently — arch_vmm_map_2m validates
     * software_bits first. */
    uint64_t sw = software_bits & AARCH64_PT_SW_ALLOWED_MASK;
    /* bit1 = 0 (block) replaces the TABLE bit set by encode_perm's
     * leaf base. */
    uint64_t desc = (base & ~AARCH64_PT_DESC_TABLE) | AARCH64_PT_DESC_VALID;
    desc |= (pa & AARCH64_PT_BLOCK_OA_MASK);
    desc |= sw;
    return desc;
}

bool aarch64_pt_range_accessible(const uint64_t *root, uint64_t va,
                                 uint64_t length, bool write, bool user)
{
    vmm_gate_check();
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
        /* rc == AARCH64_PT_OK is the only path past the previous line;
         * a block descriptor already returned ECONFLICT from query_4k. */
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
