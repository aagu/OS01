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

#include <arch/spinlock.h>        /* spinlock_T, spin_init, spin_lock /
                                     * spin_unlock (Task 17 §5.4) */
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

/* Descriptor field bits. The base AArch64 descriptor constants
 * (VALID / TABLE / AF / ATTR_NORMAL / ATTR_DEVICE / PXN / UXN) come
 * from <arch/aarch64/vmm_backend.h> — single source of truth for the
 * backend family.  Page-table-private additions (SH encoding, AP
 * decomposition, block OA mask, software-bit grouping) stay here. */
#define AARCH64_PT_DESC_SH_IS  UINT64_C(0x300)            /* bits [9:8] inner-shareable */
#define AARCH64_PT_DESC_SH_NS  UINT64_C(0x000)            /* bits [9:8] non-shareable */

/* Compile-time guard so any regression that confuses bit positions is
 * caught at build time instead of via silent memory-type drift. */
_Static_assert(AARCH64_PT_ATTR_NORMAL == 0x4,
               "AttrIndx 1 must be bit 2 = 0x4, not 0x8");
_Static_assert(AARCH64_PT_ATTR_DEVICE == 0x0,
               "AttrIndx 0 must be 0x0");

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

/* ── Page-table locks (Task 17 / spec §5.4) ─────────────────────────
 *
 * Storage + static initializers.  aarch64 spinlock_T uses 1 = unlocked
 * (spinlock.h:44) — a static zero would make the first spin_lock()
 * self-deadlock because the CAS expects 1 → 0.  The designated
 * initializers {.lock = 1UL} bypass that; arch_vmm_init() also calls
 * spin_init() on each as a belt-and-braces double insurance against
 * a future zero-init gotcha.
 *
 * Lock order (spec §5.4, total order with no reverse paths):
 *   pt_lock_for(root, l2) → pt_upper_lock → tlb_sd_lock
 *   (the third lives in kernel/memory/tlb.c — not held by this TU;
 *    the source-scan test in test_aarch64_pt_locks.c asserts no path
 *    in page_table.c takes the kernel-internal shootdown lock while
 *    both pt_lock + pt_upper_lock are held).
 *
 * (The phrase "tlb_sd_lock" above appears only in this comment; no
 * symbol in this TU references it.  The test's `assert_null(strstr(...))`
 * scan intentionally ignores the comment with a custom heuristic —
 * see test_aarch64_pt_locks.c.)
 *
 * All three are PLAIN spin_lock (not irqsave) — aarch64's
 * spin_lock_irqsave disables IRQs around the CAS spin, which would
 * block the TLB IPI handler from running on the spinning CPU and
 * deadlock the shootdown ack wait.  Waiters keep IRQs enabled. */
static spinlock_T pt_locks[64]    = { [0 ... 63] = { .lock = 1UL } };
static spinlock_T pt_upper_lock   = { .lock = 1UL };

/* Idempotent re-init: spin_init() writes 1UL to lock->lock.  Called
 * once from arch_vmm_init() so the locks are also unconditionally
 * initialised in case some future change accidentally drops the
 * static initializer. */
int aarch64_pt_init_locks(void)
{
    for (int i = 0; i < 64; i++) spin_init(&pt_locks[i]);
    spin_init(&pt_upper_lock);
    return 0;
}

/* Hash (root_pa, l2_idx) → one of 64 lock slots.  XOR-mix with a
 * 4 KiB-aligned PA so distinct L2 slots of the same root distribute;
 * mask to 63 so the slot fits the static array.  Spec §5.4: collisions
 * only hurt performance (no correctness loss — the per-slot lock is
 * the unit of serialisation either way). */
spinlock_T *pt_lock_for(uint64_t root_pa, uint32_t l2_idx)
{
    return &pt_locks[((root_pa >> 12) ^ (uint64_t)l2_idx) & 63UL];
}

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
/* walk_to_l2 is exposed via page_table.h (Task 17 §5.2b contract).
 * aarch64_pt_map_2m_block / unmap_2m_block are implemented below in
 * the "2 MiB block operations" section; their prototypes live in
 * <arch/aarch64/page_table.h>. */

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
 * releases the just-allocated page with free_4k_page().
 *
 * Lock discipline (Task 17 / spec §5.4): the call site (map_4k_ext /
 * replace_4k / query_4k_ext / unmap_4k_ext) holds pt_lock_for(root, l2)
 * AROUND the walk so the L2 slot (pmd[l2]) is stable for the
 * subsequent PTE read/write.  Inside the walk we additionally take
 * pt_upper_lock around the L0/L1 ensure segment — the global lock
 * prevents concurrent creates on two different L2 slots from racing
 * on a shared L0/L1 entry (which would either leak a table page or
 * lose a mapping, per spec §5.4).  Both locks are released before
 * returning; the call site still holds pt_lock_for (its scope is
 * wider than the walk). */
static int walk_to_l3(uint64_t *root, uint64_t va, bool create,
                      uint64_t **pte_out, int *result_out)
{
    uint64_t l0 = (va >> AARCH64_PT_L0_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l1 = (va >> AARCH64_PT_L1_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l2 = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l3 = (va >> AARCH64_PT_L3_SHIFT) & AARCH64_PT_IDX_MASK;

    uint64_t *pud = NULL, *pmd = NULL, *pte = NULL;
    int rc;

    /* Take pt_upper_lock around the L0/L1 ensure segment.  Lock order
     * is preserved because the caller already holds pt_lock_for(root, l2).
     * pt_upper_lock is released before we touch pmd[l2] / pte[l3] so
     * the L2 ensure / PTE work runs with only the L2-slot lock held. */
    spin_lock(&pt_upper_lock);
    if (ensure_child_table(root, root, l0, va, create, true,
                           &pud, &rc) != 0) {
        spin_unlock(&pt_upper_lock);
        *result_out = rc;
        return -1;
    }
    if (ensure_child_table(root, pud, l1, va, create, false,
                           &pmd, &rc) != 0) {
        spin_unlock(&pt_upper_lock);
        *result_out = rc;
        return -1;
    }
    spin_unlock(&pt_upper_lock);

    /* L2 ensure + PTE access run under pt_lock_for (held by caller). */
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
 * block path (Task 18 keeps the implementation; Task 17 introduces the
 * function + lock contract here).
 *
 * Lock contract — caller-held (matches walk_to_l3, spec §5.4):
 *   - The CALLER must hold pt_lock_for(root, l2_idx) for the L2 slot
 *     BEFORE calling walk_to_l2, and release it AFTER the caller's
 *     subsequent pmd[l2] read/write.  This guarantees the L2 slot
 *     stays stable for the caller's block-descriptor write and prevents
 *     a second caller from racing on the same slot during the walk.
 *   - walk_to_l2 internally takes ONLY pt_upper_lock around the L0/L1
 *     ensure segment — the global lock prevents two different L2
 *     slots from racing on the same L0/L1 entry (which would either
 *     leak a table page or lose a mapping, per spec §5.4).  pt_upper_lock
 *     is released before the function returns; pt_lock_for stays with
 *     the caller.
 *   - Lock order preserved: pt_lock_for (held by caller) →
 *     pt_upper_lock (taken second, released first).  This matches
 *     walk_to_l3 and the global §5.4 order pt_lock → pt_upper_lock →
 *     tlb_sd_lock.
 *
 * Earlier (Task 17 v1) walk_to_l2 acquired + released pt_lock_for
 * internally, which broke map_2m: the function had to take pt_lock_for
 * itself to write pmd[l2] but walk_to_l2 had already released it,
 * giving either (a) a self-deadlock if map_2m also tried to take it
 * after the walk, or (b) post-release slot instability if it didn't.
 * The caller-held contract here eliminates both failure modes.
 *
 * ENOMEM rollback (spec §5.2b item 3): if alloc fails at any level,
 * the just-allocated (but unpublished) page would be leaked — but the
 * alloc_4k_page() helper returns 0 on failure BEFORE we call
 * zero_page() / write parent[index], so there's literally nothing to
 * free at the failing level.  Previously-published intermediate tables
 * (e.g. the L0 page already published when L1's alloc fails) are
 * KEPT — they're harmless empty tables, reusable by future callers,
 * and M3 has no intermediate-table reclaim (same cost class as F1).
 * The caller retains pt_lock_for across the ENOMEM return so its own
 * rollback (if any) is consistent.
 *
 * With create == false returns OK only when every level is present;
 * otherwise sets *result_out to ENOENT, EINVAL, or ECONFLICT and
 * returns -1.  Block descriptors at L2 are NOT rejected here — the
 * caller checks `pmd[l2]` to distinguish block vs table for split.
 *
 * Deferred (spec §5.2b item 4 "每级发布后 local TLBI"): when a caller
 * first walks a PUBLISHED root with create=true (no such caller today
 * — every current create-path caller runs before its root is
 * published), this function must branch on `create &&
 * is_active_root(root)` and issue a per-level local TLBI as each
 * intermediate table is published.  Until such a caller exists the
 * branch stays a no-op by design. */
int walk_to_l2(uint64_t *root, uint64_t va, bool create,
               uint64_t **pmd_out, int *result_out)
{
    uint64_t l0 = (va >> AARCH64_PT_L0_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l1 = (va >> AARCH64_PT_L1_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t l2 = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    /* `create` is consulted by ensure_child_table (per-level alloc);
     * the per-level TLBI branch on `create && is_active_root(root)`
     * lands here when the first published-root create-path caller
     * arrives (see the comment above). */
    (void)l2;  /* used by ensure_child_table via the parent pointer */
    (void)create;

    /* Lock order: caller already holds pt_lock_for(root, l2).  We
     * take only pt_upper_lock around the L0/L1 ensure segment —
     * matching walk_to_l3's caller-held pt_lock pattern. */
    spin_lock(&pt_upper_lock);

    uint64_t *pud = NULL, *pmd = NULL;
    int rc;

    if (ensure_child_table(root, root, l0, va, create, true,
                           &pud, &rc) != 0) {
        spin_unlock(&pt_upper_lock);
        *result_out = rc;
        return -1;
    }
    if (ensure_child_table(root, pud, l1, va, create, false,
                           &pmd, &rc) != 0) {
        spin_unlock(&pt_upper_lock);
        *result_out = rc;
        return -1;
    }

    spin_unlock(&pt_upper_lock);

    *pmd_out = pmd;
    *result_out = AARCH64_PT_OK;
    return 0;
}

/* ── Test-observation hooks (weak defaults; hosttests override) ──
 *
 * Spec §4.4.3 row 1 (perm-only) takes an atomic 8 B store; rows 2-4
 * (memtype / PA / validity-flip) take the PTE-level BBM sequence.
 * The branch is the contract — hosttests need to observe which
 * branch fired without reading the descriptor (the result is the
 * same in both cases).  The weak-hook pattern matches vmm_gate.c's
 * vmm_gate_violation(): production code links the default no-op,
 * hosttests override with counters. */
__attribute__((weak)) void aarch64_pt_test_note_atomic_replace(void) {}
__attribute__((weak)) void aarch64_pt_test_note_bbm_replace(void)   {}

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

/* Brief-aligned alias for aarch64_pt_map_4k_ext.  The `ext` variant
 * is the authoritative implementation; this alias exists so callers
 * (and the hosttest 12-legal + 4-reject matrix) can use the brief's
 * `map_4k_new` naming.  Contract identical — returns -EEXIST on ANY
 * occupied slot (valid leaf OR PROTNONE-stashed). */
int aarch64_pt_map_4k_new(uint64_t *root, uint64_t va, uint64_t pa,
                          uint32_t perm, uint64_t software_bits)
{
    return aarch64_pt_map_4k_ext(root, va, pa, perm, software_bits);
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

    /* L2-slot lock (Task 17 / spec §5.4).  Held around the whole walk
     * + PTE-store so the L2 slot stays stable and the slot's leaf
     * read-then-write is atomic w.r.t. other CPUs touching the same
     * 2 MiB region.  walk_to_l3 internally takes pt_upper_lock for
     * the L0/L1 ensure segment — order pt_lock_for → pt_upper_lock
     * is preserved. */
    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);
    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);
    spin_lock(pt_lock);

    uint64_t *pte = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l3(root, va, true, &pte, &wr);
    if (wrc != 0) { spin_unlock(pt_lock); return wr; }

    if ((*pte & AARCH64_PT_DESC_VALID) != 0) {
        spin_unlock(pt_lock);
        return AARCH64_PT_EEXIST;
    }
    /* PROTNONE stash (VALID clear but PROTNONE bit set) is also
     * "occupied" — caller must unmap first. */
    if ((*pte & AARCH64_PT_SOFTWARE_PROTNONE) != 0) {
        spin_unlock(pt_lock);
        return AARCH64_PT_EEXIST;
    }

    *pte = desc;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    spin_unlock(pt_lock);
    return AARCH64_PT_OK;
}

int aarch64_pt_query_4k_ext(const uint64_t *root, uint64_t va,
                            uint64_t *pa_out, uint32_t *perm_out,
                            uint64_t *sw_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;

    /* L2-slot lock held around the walk + leaf read so concurrent
     * updates (map_4k_ext / replace_4k / unmap_4k_ext) can't change
     * the descriptor between read_leaf and the decode step. */
    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);
    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);
    spin_lock(pt_lock);

    uint64_t *r = (uint64_t *)root;
    uint64_t *pte = NULL;
    uint64_t desc = 0;
    int wr = AARCH64_PT_OK;
    int rc = read_leaf(r, va, false, &pte, &desc, &wr);
    if (rc != 0) { spin_unlock(pt_lock); return wr; }

    if (wr == AARCH64_PT_EPROT_NONE) {
        if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
        if (perm_out) *perm_out = 0;
        if (sw_out)   *sw_out   = AARCH64_PT_SOFTWARE_PROTNONE;
        spin_unlock(pt_lock);
        return AARCH64_PT_EPROT_NONE;
    }
    if (wr == AARCH64_PT_ENOENT) {
        spin_unlock(pt_lock);
        return AARCH64_PT_ENOENT;
    }

    /* Valid leaf: decode perm + sw directly from the descriptor. */
    uint32_t perm;
    uint64_t sw;
    rv = decode_perm(desc, &perm, &sw);
    if (rv != AARCH64_PT_OK) { spin_unlock(pt_lock); return rv; }
    if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
    if (perm_out) *perm_out = perm;
    if (sw_out)   *sw_out   = sw;
    spin_unlock(pt_lock);
    return AARCH64_PT_OK;
}

int aarch64_pt_unmap_4k_ext(uint64_t *root, uint64_t va,
                            uint64_t *pa_out, uint32_t *perm_out,
                            uint64_t *sw_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;

    /* L2-slot lock around the leaf read + clear. */
    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);
    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);
    spin_lock(pt_lock);

    uint64_t *pte = NULL;
    uint64_t desc = 0;
    int wr = AARCH64_PT_OK;
    int rc = read_leaf(root, va, false, &pte, &desc, &wr);
    if (rc != 0) { spin_unlock(pt_lock); return wr; }

    if (wr == AARCH64_PT_EPROT_NONE) {
        if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
        if (perm_out) *perm_out = 0;
        if (sw_out)   *sw_out   = AARCH64_PT_SOFTWARE_PROTNONE;
        /* Clear the slot; the page is owned by the caller (spec
         * §4.4.4) so we do NOT free_4k_page the data PA. */
        *pte = 0;
        dsb_ishst();
        if (is_active_root(root)) tlb_invalidate_local(va);
        spin_unlock(pt_lock);
        return AARCH64_PT_EPROT_NONE;
    }
    if (wr == AARCH64_PT_ENOENT) {
        spin_unlock(pt_lock);
        return AARCH64_PT_ENOENT;
    }

    uint32_t perm;
    uint64_t sw;
    rv = decode_perm(desc, &perm, &sw);
    if (rv != AARCH64_PT_OK) { spin_unlock(pt_lock); return rv; }
    if (pa_out)   *pa_out   = desc & AARCH64_PT_PA_MASK;
    if (perm_out) *perm_out = perm;
    if (sw_out)   *sw_out   = sw;

    *pte = 0;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    spin_unlock(pt_lock);
    return AARCH64_PT_OK;
}

int aarch64_pt_replace_4k(uint64_t *root, uint64_t va, uint64_t pa,
                          uint32_t perm, uint64_t software_bits,
                          uint64_t *old_pa_out, uint32_t *old_perm_out,
                          uint64_t *old_sw_out)
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

    /* L2-slot lock around the leaf read + descriptor rewrite.  Holds
     * across the atomic-store / BBM branch so the slot stays stable
     * and the read→classify→write is atomic w.r.t. other CPUs. */
    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);
    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);
    spin_lock(pt_lock);

    uint64_t *pte = NULL;
    uint64_t old_desc = 0;
    int wr = AARCH64_PT_OK;
    int rc = read_leaf(root, va, false, &pte, &old_desc, &wr);
    if (rc != 0) { spin_unlock(pt_lock); return wr; }
    if (wr == AARCH64_PT_ENOENT) {
        spin_unlock(pt_lock);
        return AARCH64_PT_ENOENT;
    }

    /* Always report the prior state so callers can reconstruct
     * (VMA → COW, COW → VMA, etc.) regardless of the category.
     * old_perm is 0 when the prior slot was a PROTNONE stash (the
     * stash carries no live permission). */
    uint64_t old_pa;
    uint64_t old_sw;
    uint32_t old_perm;
    bool was_valid;
    if (wr == AARCH64_PT_EPROT_NONE) {
        old_pa   = old_desc & AARCH64_PT_PA_MASK;
        old_sw   = AARCH64_PT_SOFTWARE_PROTNONE;
        old_perm = 0;
        was_valid = false;
    } else {
        rv = decode_perm(old_desc, &old_perm, &old_sw);
        if (rv != AARCH64_PT_OK) { spin_unlock(pt_lock); return rv; }
        old_pa = old_desc & AARCH64_PT_PA_MASK;
        was_valid = true;
    }

    /* Spec §4.4.3 classification: perm-only is "same PA, same memory
     * type, same validity" — AP[2:1] / PXN / UXN are the ONLY bits
     * permitted to differ (they are the "perm bits" the row names).
     * The previous round's `same_rest` comparison demanded every
     * non-PA-non-sw bit be identical, which made perm_only=false for
     * any actual perm change (RW→RO → AP bits differ → BBM path) and
     * the atomic-store fast path unreachable except for no-op writes.
     * Drop same_rest entirely: PA + AttrIndx + validity equality is
     * the precise row-1 contract.  PROTNONE↔VALID flips are caught by
     * same_valid and fall through to the BBM branch (row 4). */
    bool same_pa      = (old_pa == pa);
    bool same_attr    = ((old_desc & (UINT64_C(0x7) << 2)) ==
                         (new_desc & (UINT64_C(0x7) << 2)));
    bool same_valid   = (was_valid ==
                         ((software_bits & AARCH64_PT_SOFTWARE_PROTNONE) == 0));
    bool perm_only    = same_pa && same_attr && same_valid;

    /* Spec §4.4.3 row 1 (perm-only) → atomic store + dsb + local
     * TLBI. Rows 2-4 (memtype / PA / validity) → PTE-level BBM:
     * clear → dsb → TLBI → set → dsb → TLBI.  Both branches run
     * under pt_lock_for(root, l2) — see brief / spec §5.4. */
    if (perm_only) {
        __atomic_store_n(pte, new_desc, __ATOMIC_SEQ_CST);
        dsb_ishst();
        tlb_invalidate_local(va);
        aarch64_pt_test_note_atomic_replace();
    } else {
        /* PTE-level BBM. */
        *pte = 0;
        dsb_ishst();
        tlb_invalidate_local(va);

        *pte = new_desc;
        dsb_ishst();
        tlb_invalidate_local(va);
        aarch64_pt_test_note_bbm_replace();
    }

    if (old_pa_out)   *old_pa_out   = old_pa;
    if (old_perm_out) *old_perm_out = old_perm;
    if (old_sw_out)   *old_sw_out   = old_sw;
    spin_unlock(pt_lock);
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

/* ── 2 MiB block operations (Task 18 / spec §5.2 / §4.4.5) ─────────── */

/* Map a 2 MiB block descriptor at pmd[l2].  L2-slot lock held across
 * the walk + pmd[l2] write so a concurrent caller cannot race on the
 * same slot.  Rejects:
 *   - misaligned VA/PA, PA >= 1 TiB, unrecognised perm → -EINVAL
 *   - any occupied pmd[l2] (valid table, valid block, OR PROTNONE
 *     stash on the block format) → -EEXIST
 *   - missing intermediate table page → -ENOMEM (kept-published
 *     intermediates are harmless and reusable — spec §5.2b)
 *
 * Software bits are NOT supported on block descriptors in this
 * commit (the brief's 12-legal combos don't include a PROTNONE-block
 * path; a follow-up can extend the contract if a real caller needs
 * it).  The perm validation goes through encode_perm so DEVICE |
 * EXEC and bad RO/RW/kernel/user combos fail before any descriptor
 * is written. */
int aarch64_pt_map_2m_block(uint64_t *root, uint64_t va, uint64_t pa,
                            uint32_t perm)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_2M_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if ((pa & (PAGE_2M_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;
    if (pa >= AARCH64_PT_PA_LIMIT)       return AARCH64_PT_EINVAL;

    /* Strict perm validation: encode_perm rejects bad combos.  The
     * block desc encoder (encode_block_desc) silently falls back to
     * a kernel-RO block on bad perm; we want a hard -EINVAL instead. */
    uint64_t base;
    rv = encode_perm(perm, &base);
    if (rv != AARCH64_PT_OK) return rv;

    /* Build the full descriptor.  No software bits for blocks yet. */
    uint64_t desc = aarch64_pt_encode_block_desc(pa, perm, 0);

    /* L2-slot lock around walk + pmd[l2] write (spec §5.2 / §5.4).
     * Caller-held pattern — walk_to_l2 internally takes pt_upper_lock
     * around L0/L1 ensure, releasing before returning; we hold
     * pt_lock_for across the whole sequence. */
    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);
    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);
    spin_lock(pt_lock);

    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2(root, va, true, &pmd, &wr);
    if (wrc != 0) { spin_unlock(pt_lock); return wr; }

    uint64_t cur = pmd[l2_idx];
    /* Spec §5.2: "pmd[l2] already occupied → -EEXIST".  Any valid
     * desc (block or table) counts as occupied; the PROTNONE software
     * stash on a block format is also occupied (the next unmap
     * would need to clear it first). */
    if ((cur & AARCH64_PT_DESC_VALID) != 0 ||
        (cur & AARCH64_PT_SOFTWARE_PROTNONE) != 0) {
        spin_unlock(pt_lock);
        return AARCH64_PT_EEXIST;
    }

    pmd[l2_idx] = desc;
    dsb_ishst();
    if (is_active_root(root)) {
        /* Per-VA TLBI is sufficient for a block — the TLB entry that
         * caches the block contains the base VA, so vae1(base_va)
         * invalidates the whole 2 MiB entry.  (Arm ARM TLBI VAE1
         * invalidates by VA regardless of granule.) */
        tlb_invalidate_local(va);
    }
    spin_unlock(pt_lock);
    return AARCH64_PT_OK;
}

/* Unmap a 2 MiB block (spec §4.4.5).  Reads pmd[l2] and dispatches:
 *   - block (V=1, bit1=0) → return PA via *pa_out, clear entry,
 *                            local TLBI, return OK.
 *   - table (V=1, bit1=1) → -EINVAL (type mismatch; caller used the
 *                            wrong API — a 4 KiB leaf unmap is
 *                            aarch64_pt_unmap_4k_ext).
 *   - invalid (V=0)       → -ENOENT.
 *
 * L2-slot lock held across the read+write per spec §5.4.  Never
 * frees the data page (backend owns no data pages — spec §4.4.4). */
int aarch64_pt_unmap_2m_block(uint64_t *root, uint64_t va,
                              uint64_t *pa_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_2M_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;

    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);
    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);
    spin_lock(pt_lock);

    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2(root, va, false, &pmd, &wr);
    if (wrc != 0) { spin_unlock(pt_lock); return wr; }

    uint64_t desc = pmd[l2_idx];
    if ((desc & AARCH64_PT_DESC_VALID) == 0) {
        spin_unlock(pt_lock);
        return AARCH64_PT_ENOENT;
    }
    if ((desc & AARCH64_PT_DESC_TABLE) != 0) {
        spin_unlock(pt_lock);
        return AARCH64_PT_EINVAL;
    }

    /* Block descriptor — extract PA bits [39:21] (L2 block format),
     * clear the slot, dsb, and issue per-VA local TLBI if active. */
    if (pa_out) *pa_out = desc & AARCH64_PT_BLOCK_OA_MASK;
    pmd[l2_idx] = 0;
    dsb_ishst();
    if (is_active_root(root)) tlb_invalidate_local(va);
    spin_unlock(pt_lock);
    return AARCH64_PT_OK;
}

/* Split a 2 MiB block into 512 4 KiB leaves (spec §5.3).
 *
 * Unpublished roots (caller owns the tree exclusively) take a single
 * atomic 8 B store rewrite: build the L3 page in place and replace
 * the block desc at pmd[l2] with a table desc. No TLBI needed — no
 * other CPU can have cached the translation of an unpublished root.
 *
 * Published roots return -EPERM (F10 implements the BBM-aware split
 * path with per-CPU TLBI invalidation; the block↔table swap on a live
 * root depends on ID_AA64MMFR2_EL1.BBM support level and requires
 * cross-core invalidation after publication).
 *
 * Lock contract (matches walk_to_l2 / map_2m_block / unmap_2m_block):
 *   - Caller-held pt_lock_for(root, l2_idx) across walk + pmd[l2]
 *     write — same pattern §5.4 mandates for the rest of the block
 *     path.
 *   - pt_upper_lock is taken internally by walk_to_l2 around the
 *     L0/L1 ensure segment; released before walk_to_l2 returns.
 *
 * Failure contract (spec §5.3):
 *   - Step 0 (registry check, outside pt_lock): published → -EPERM.
 *     Caller hasn't allocated anything yet; original block is intact.
 *   - Step 1 (l3_pa alloc): 0 → -ENOMEM; original block untouched.
 *   - Step 4 (lock + re-read pmd[l2]):
 *     !VALID              → unlock + free l3_pa + -ENOENT.
 *     TYPE_TABLE (V=1, b1=1) → unlock + free l3_pa + -EAGAIN
 *                               (concurrent split caller won; retry).
 *   - Steps 5–8 succeed unconditionally; no allocation happens after
 *     step 5 so there are no later failure paths.
 */
int aarch64_pt_split_block_2m(uint64_t *root, uint64_t va)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_2M_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;

    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    uint64_t root_pa = (uint64_t)((uintptr_t)root - (uintptr_t)ARCH_PAGE_OFFSET);

    /* Step 0: root lifecycle check.  Outside pt_lock — the registry
     * has its own lock, and registration only happens at install /
     * arch_switch_mm paths (no contention with split).  The registry
     * takes a pointer to the PA (not to the page itself) — see
     * <arch/aarch64/vmm_gate.h> for the contract; passing `root`
     * here would dereference PGD[0] instead of the PA. */
    if (aarch64_pt_root_is_published(&root_pa)) return AARCH64_PT_EPERM;

    spinlock_T *pt_lock = pt_lock_for(root_pa, (uint32_t)l2_idx);

    /* Step 1: allocate the L3 page BEFORE locking.  alloc_4k_page()
     * returns 0 on failure BEFORE we touch anything, so a -ENOMEM
     * here leaves the original block mapping completely intact. */
    uint64_t l3_pa = alloc_4k_page();
    if (l3_pa == 0) return AARCH64_PT_ENOMEM;

    /* Step 2: zero the L3 page so the uninitialised PTEs can't leak
     * stale bits to a future reader. */
    zero_page(l3_pa);

    /* Step 3: lock pt_lock_for(root, l2).  Caller-held pattern —
     * walk_to_l2 internally takes pt_upper_lock around the L0/L1
     * ensure, releasing before returning.  We hold pt_lock_for
     * across the walk + pmd[l2] write so no other caller can race
     * on the slot. */
    spin_lock(pt_lock);

    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2(root, va, false, &pmd, &wr);
    if (wrc != 0) {
        /* walk_to_l2 failed (e.g. an intermediate table was missing
         * because create=false and a higher-level page was never
         * allocated).  Free the L3 page we just allocated and
         * release the lock. */
        spin_unlock(pt_lock);
        free_4k_page(l3_pa);
        return wr;
    }

    /* Step 4: re-read pmd[l2] under the lock and dispatch. */
    uint64_t d = pmd[l2_idx];
    if ((d & AARCH64_PT_DESC_VALID) == 0) {
        spin_unlock(pt_lock);
        free_4k_page(l3_pa);
        return AARCH64_PT_ENOENT;
    }
    if ((d & AARCH64_PT_DESC_TABLE) != 0) {
        /* Slot is already a valid TABLE descriptor — a concurrent
         * split caller won the race.  Per spec the caller retries. */
        spin_unlock(pt_lock);
        free_4k_page(l3_pa);
        return AARCH64_PT_EAGAIN;
    }

    /* Step 5: build the 512 leaves in the freshly-zeroed L3 page.
     *
     * The block descriptor holds:
     *   - V=1, bit1=0 (block type)
     *   - PA bits [39:21]  (block OA field)
     *   - AP[2:1] bits [7:6], SH bits [9:8], AttrIndx bits [4:2],
     *     AF bit 10, PXN bit 53, UXN bit 54
     *   - software bits bit55/56 (PROTNONE / COW)
     *
     * Each leaf must carry the SAME attribute bits (AP/SH/AttrIndx/
     * AF/PXN/UXN/SW) but with V=1, bit1=1 (leaf type) and PA bits
     * [39:12] = block_pa + i*PAGE_4K_SIZE.
     *
     * The cleanest transform is bit-level: take the block desc,
     * OR in the TABLE bit (so leaf type replaces block type), and
     * replace the PA field with the per-leaf PA.  V=1 is already
     * set in d (the source block is by definition valid). */
    uint64_t block_pa = d & AARCH64_PT_BLOCK_OA_MASK;
    uint64_t leaf_base = (d | AARCH64_PT_DESC_TABLE) & ~AARCH64_PT_PA_MASK;
    volatile uint64_t *pte = (volatile uint64_t *)(l3_pa + ARCH_PAGE_OFFSET);
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t leaf_pa = (block_pa + (uint64_t)i * PAGE_4K_SIZE) &
                            AARCH64_PT_PA_MASK;
        pte[i] = leaf_base | leaf_pa;
    }
    dsb_ishst();

    /* Step 6: the single atomic 8 B store — pmd[l2] becomes a valid
     * table descriptor pointing to the new L3 page.  Replaces the
     * block desc without any intermediate state visible to readers
     * (the L3 page was already populated before this store). */
    pmd[l2_idx] = encode_table_desc(l3_pa);

    /* Step 7: dsb ishst so the L3 stores are ordered before the pmd[l2]
     * store is observable to other agents.  No TLBI: unpublished
     * root means no other CPU can have cached the old block desc. */
    dsb_ishst();

    /* Step 8: unlock. */
    spin_unlock(pt_lock);

    return AARCH64_PT_OK;
}

/* Read the raw descriptor at pmd[l2] for `va`.  Walks L0 → L1 → L2
 * without allocating and returns pmd[l2_idx].  Test helper; see
 * <arch/aarch64/page_table.h> for the full contract. */
int aarch64_pt_read_l2_desc(const uint64_t *root, uint64_t va,
                            uint64_t *desc_out)
{
    vmm_gate_check();
    int rv = root_valid(root);
    if (rv != AARCH64_PT_OK) return rv;
    if (desc_out == NULL) return AARCH64_PT_EINVAL;
    if (!va_canonical(va)) return AARCH64_PT_EINVAL;
    if ((va & (PAGE_2M_SIZE - 1)) != 0) return AARCH64_PT_EINVAL;

    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2((uint64_t *)root, va, false, &pmd, &wr);
    if (wrc != 0) return wr;

    uint64_t l2_idx = (va >> AARCH64_PT_L2_SHIFT) & AARCH64_PT_IDX_MASK;
    *desc_out = pmd[l2_idx];
    return AARCH64_PT_OK;
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
