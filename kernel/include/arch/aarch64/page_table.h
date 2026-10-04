/* kernel/include/arch/aarch64/page_table.h
 *
 * AArch64 stage-1 translation-table primitive layer.
 *
 * Owns all descriptor encoding/decoding for 4 KiB page leaves plus the
 * intermediate table allocation that links them. Caller-supplied root
 * pointer; the existing active root obtained via arch_get_page_table()
 * is the only "active" root in this increment.
 *
 * Scope (binding — see design §"Non-goals"):
 *   - 4 KiB leaves only; L1/L2 block descriptors installed by boot code
 *     are reported as ECONFLICT and never modified.
 *   - caller must validate root, VA, PA against the constants below;
 *     invalid inputs return EINVAL before any table modification.
 *   - active-root use is BSP/pre-SMP-only; APs must not call map/unmap.
 *   - intermediate tables are NOT reclaimed in this increment; the only
 *     page freed on a path that fails AFTER allocating but BEFORE
 *     linking is the page itself (one entry).
 *   - no generic VMM, no x86 PAGE_* flags, no EL0 entry.
 */

#ifndef OS01_AARCH64_PAGE_TABLE_H
#define OS01_AARCH64_PAGE_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#include <arch/spinlock.h>        /* spinlock_T (Task 17 §5.4) */

/* High-half kernel-self-test VA. Its lower-48-bit L0 slot is absent from
 * the boot tables at the time of the BSP self-test (TTBR0_EL1 == TTBR1_EL1
 * == boot_page_tables; the corresponding PGD[256] entry is invalid), so a
 * `aarch64_pt_query_4k()` against this VA must initially return ENOENT. The
 * slot is not a permanent kernel-private address — the explicit unmap in
 * the smoke test ensures no leaf is left behind. */
#define AARCH64_PT_SELFTEST_VA UINT64_C(0xffff800000000000)

/* TTBR0_EL1 layout. The base address field is bits [47:12]; the
 * permitted non-base bits are the ASID (bits [63:48]) and the CnP bit
 * (bit 0). All other bits must read as zero on the live TTBR. Exposed
 * here so the BSP pre-SMP self-test can validate the active root's
 * raw TTBR0_EL1 value before converting to a direct-map pointer. */
#define AARCH64_TTBR_BASE_MASK       UINT64_C(0x000000fffffff000)
#define AARCH64_TTBR_ALLOWED_NONBASE (UINT64_C(0xffff000000000000) | \
                                      UINT64_C(1))
#define AARCH64_TTBR_ALLOWED_MASK    (AARCH64_TTBR_BASE_MASK | \
                                      AARCH64_TTBR_ALLOWED_NONBASE)

/* Permission word (bit-flag style). The caller MUST set exactly one
 * kernel/user flag and exactly one RO/RW flag, may optionally add
 * EXEC and/or DEVICE. Undefined bit combinations are EINVAL; the
 * combination DEVICE | EXEC is always rejected. */
enum aarch64_pt_perm {
    AARCH64_PT_KERNEL_RO = 1u << 0,
    AARCH64_PT_KERNEL_RW = 1u << 1,
    AARCH64_PT_USER_RO   = 1u << 2,
    AARCH64_PT_USER_RW   = 1u << 3,
    AARCH64_PT_EXEC      = 1u << 4,
    AARCH64_PT_DEVICE    = 1u << 5,
};

/* Result codes. Negative values are errors; AARCH64_PT_OK is 0.
 * AARCH64_PT_EPROT_NONE is the three-state query sentinel for an
 * "invalid but holds a PA" software stash (VM_PRESENT=0, VM_PROTNONE=1);
 * identical code lives in <arch/aarch64/vmm_backend.h> as a `#define`
 * (the backend includes both headers and uses the negative literal). */
enum aarch64_pt_result {
    AARCH64_PT_OK        =  0,
    AARCH64_PT_EINVAL    = -1,
    AARCH64_PT_EEXIST    = -2,
    AARCH64_PT_ENOENT    = -3,
    AARCH64_PT_ENOMEM    = -4,
    AARCH64_PT_ECONFLICT = -5,
};

/* Map a single 4 KiB page at the given VA in `root`. `va` and `pa`
 * must be 4 KiB aligned; `pa` must be < 1 TiB. Missing intermediate
 * tables are allocated via alloc_4k_page() and zeroed before linking.
 *
 * Thin wrapper over aarch64_pt_map_4k_ext() with software_bits = 0;
 * retained so the BSP pre-SMP selftest (kernel/arch/aarch64/memory/
 * m1_selftest.c) keeps using its 4-arg form unchanged.  See the ext
 * variant for the full return-value contract. */
int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                      uint32_t perm);

/* M3.3 (Task 16) full 5-arg primitive with software-bit support.  Map
 * a 4 KiB leaf with perm AND software_bits (PROTNONE | COW — both bits
 * set is rejected with -EINVAL).
 *
 * Returns:
 *   AARCH64_PT_OK        on success.
 *   AARCH64_PT_EINVAL    for null root, misaligned/uncanonical VA/PA,
 *                        PA >= 1 TiB, unknown permission bits,
 *                        DEVICE | EXEC, or software_bits with bits set
 *                        outside {PROTNONE, COW}, or both bits set.
 *   AARCH64_PT_EEXIST    when a leaf (valid OR PROTNONE-stashed) is
 *                        already present at VA.
 *   AARCH64_PT_ECONFLICT when a valid non-table PUD/PMD descriptor
 *                        (block entry) is encountered.
 *   AARCH64_PT_ENOMEM    when a 4 KiB table page cannot be allocated.
 *
 * Active-root callers may invoke this only before smp_boot_aps(). */
int aarch64_pt_map_4k_ext(uint64_t *root, uint64_t va, uint64_t pa,
                          uint32_t perm, uint64_t software_bits);

/* Walk the tree and report the leaf at VA without allocating.
 * Three-state return (spec §4.3 query_4k):
 *   AARCH64_PT_OK         — valid mapping; `*pa_out` / `*perm_out` /
 *                           `*sw_out` receive decoded state.
 *   AARCH64_PT_EPROT_NONE — PROTNONE-stashed: VALID cleared but the
 *                           PROTNONE software bit set; `*pa_out` holds
 *                           the stashed PA.  Distinct from -ENOENT.
 *   AARCH64_PT_ENOENT     — slot genuinely empty.
 *
 * Thin wrapper over aarch64_pt_query_4k_ext() that drops the sw out
 * parameter.  Retained for the BSP pre-SMP selftest. */
int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out);

/* Three-state query + software-bit out (see aarch64_pt_query_4k for the
 * return-value contract).  `*sw_out` is populated with the raw software
 * bits at the descriptor (PROTNONE / COW / neither). */
int aarch64_pt_query_4k_ext(const uint64_t *root, uint64_t va,
                            uint64_t *pa_out, uint32_t *perm_out,
                            uint64_t *sw_out);

/* Clear an existing 4 KiB leaf. Returns the prior physical address and
 * permission via `*pa_out` / `*perm_out` before the clear.  Same
 * three-state return as aarch64_pt_query_4k_ext: PROTNONE stashes are
 * cleared and reported as AARCH64_PT_EPROT_NONE.  Never allocates. */
int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out);

/* Three-state clear + software-bit out. */
int aarch64_pt_unmap_4k_ext(uint64_t *root, uint64_t va,
                            uint64_t *pa_out, uint32_t *perm_out,
                            uint64_t *sw_out);

/* Replace an existing 4 KiB leaf with a new perm + software_bits state
 * (spec §4.4.3).  The prior physical address, decoded permission
 * word, and software bits are returned via `*old_pa_out` /
 * `*old_perm_out` / `*old_sw_out` (all may be NULL).  Classification:
 *
 *   - AP / XN-only change (same PA, same memory type, same validity):
 *     atomic 8 B store + dsb ishst + local TLBI.
 *   - Memory-type change (AttrIndx / NOCACHE), PA change, or validity
 *     flip (↔ PROTNONE): PTE-level BBM (clear, TLBI, set) under the
 *     lock-free single-threaded assumption that Task 17 will replace
 *     with pt_lock_for(root, l2) — see TODO.
 *
 * Returns AARCH64_PT_OK on success, -EINVAL for bad perm/sw, -ENOENT
 * if no leaf (valid OR PROTNONE-stashed) exists at VA. */
int aarch64_pt_replace_4k(uint64_t *root, uint64_t va, uint64_t pa,
                          uint32_t perm, uint64_t software_bits,
                          uint64_t *old_pa_out, uint32_t *old_perm_out,
                          uint64_t *old_sw_out);

/* Build a 2 MiB block descriptor (spec §5.1).  Block = VALID | bit1=0;
 * OA lives in bits [39:21] (L2 block format — IPS=40).  AP / SH /
 * AttrIndx / XN bits mirror encode_perm's policy for the same perm
 * word.  software_bits stashed into descriptor bits 55 / 56 (the
 * reserved bits ignored by hardware).  Helper for the M3.3 / M3.4
 * arch_vmm_map_2m backend; visible here so hosttest pins the contract. */
uint64_t aarch64_pt_encode_block_desc(uint64_t pa, uint32_t perm,
                                      uint64_t software_bits);

/* Return true iff every page in [va, va + length) is mapped with the
 * requested access. length == 0 returns true. addr + length overflow
 * returns false. Requests a writable mapping when `write` is true and
 * a user-accessible mapping when `user` is true. Never allocates.
 *
 * In this increment the layer only owns 4 KiB leaves; any PUD/PMD
 * block entry seen on the path returns false (conservative). */
bool aarch64_pt_range_accessible(const uint64_t *root, uint64_t va,
                                uint64_t length, bool write, bool user);

/* Test-observation hooks for replace_4k branch selection (spec
 * §4.4.3 row 1 = perm-only atomic-store; rows 2-4 = BBM).  Defined
 * here so hosttests can override them with counters and pin the
 * classification logic; production builds link the default weak
 * no-op stubs in page_table.c, which cost nothing at -O2.  The
 * M3.1 audit-criterion pattern (weak default spin, hosttest
 * override) is the same shape vmm_gate.c uses for
 * vmm_gate_violation(). */
void aarch64_pt_test_note_atomic_replace(void);
void aarch64_pt_test_note_bbm_replace(void);

/* ── Page-table locks (Task 17 / spec §5.4) ─────────────────────────
 *
 * Three plain spin_locks (NOT irqsave — aarch64 spin_lock_irqsave
 * blocks SGI response and would deadlock the TLB ack wait):
 *
 *   pt_locks[64]        per-L2-slot hash locks
 *   pt_upper_lock       global "creating L0/L1" lock
 *   tlb_sd_lock         (Task 12, defined in kernel/memory/tlb.c)
 *
 * Total lock order: pt_lock → pt_upper_lock → tlb_sd_lock (no reverse
 * paths; TLB IPI handler takes none).  See page_table.c for the
 * storage and init_locks() / pt_lock_for() definitions; spec §5.4
 * is the authoritative contract. */

/* Idempotent re-init for pt_locks[64] + pt_upper_lock (writes 1UL to
 * each lock->lock).  Called from arch_vmm_init() as a belt-and-braces
 * double insurance against any future accidental zero-init. */
int aarch64_pt_init_locks(void);

/* Hash (root_pa, l2_idx) → one of 64 lock slots.  Same input always
 * returns the same slot; distinct inputs may collide (no correctness
 * loss, only contention).  root_pa is the translation root's PA (the
 * caller subtracts ARCH_PAGE_OFFSET from the kernel-half pointer). */
spinlock_T *pt_lock_for(uint64_t root_pa, uint32_t l2_idx);

/* Walk L0 → L1 → L2 for `va` (no descent to L3).  Returns the L2
 * (PMD) table's direct-map pointer via `*pmd_out`.  Lock order:
 * pt_lock_for(root, l2) → pt_upper_lock, both released on return.
 *
 * ENOMEM contract (spec §5.2b item 3): if alloc fails at any level,
 * the failing level's just-allocated page is freed (none — alloc
 * returned 0 BEFORE we touched anything); previously-published empty
 * intermediate tables are KEPT (reusable, harmless, M3 has no
 * reclaim — same cost class as F1).  Returns -1 with `*result_out =
 * AARCH64_PT_ENOMEM` on alloc failure, -1 with `*result_out =
 * AARCH64_PT_ENOENT` for create=false with a missing level, and 0
 * with `*result_out = AARCH64_PT_OK` on success.
 *
 * Used by the 2 MiB block path (Task 18 keeps the contract, lands the
 * implementation).  Hosttest test_aarch64_pt_locks.c exercises this
 * signature directly. */
int walk_to_l2(uint64_t *root, uint64_t va, bool create,
               uint64_t **pmd_out, int *result_out);

#endif /* OS01_AARCH64_PAGE_TABLE_H */