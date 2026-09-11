/* kernel/include/kernel/arch/aarch64/page_table.h
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

/* High-half kernel-self-test VA. Its lower-48-bit L0 slot is absent from
 * the boot tables at the time of the BSP self-test (TTBR0_EL1 == TTBR1_EL1
 * == boot_page_tables; the corresponding PGD[256] entry is invalid), so a
 * `aarch64_pt_query_4k()` against this VA must initially return ENOENT. The
 * slot is not a permanent kernel-private address — the explicit unmap in
 * the smoke test ensures no leaf is left behind. */
#define AARCH64_PT_SELFTEST_VA UINT64_C(0xffff800000000000)

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

/* Result codes. Negative values are errors; AARCH64_PT_OK is 0. */
enum aarch64_pt_result {
    AARCH64_PT_OK       =  0,
    AARCH64_PT_EINVAL   = -1,
    AARCH64_PT_EEXIST   = -2,
    AARCH64_PT_ENOENT   = -3,
    AARCH64_PT_ENOMEM   = -4,
    AARCH64_PT_ECONFLICT = -5,
};

/* Map a single 4 KiB page at the given VA in `root`. `va` and `pa`
 * must be 4 KiB aligned; `pa` must be < 1 TiB. Missing intermediate
 * tables are allocated via alloc_4k_page() and zeroed before linking.
 *
 * Returns:
 *   AARCH64_PT_OK        on success.
 *   AARCH64_PT_EINVAL    for null root, misaligned/uncanonical VA/PA,
 *                        PA >= 1 TiB, unknown permission bits, or
 *                        DEVICE | EXEC.
 *   AARCH64_PT_EEXIST    when a leaf is already present at VA.
 *   AARCH64_PT_ECONFLICT when a valid non-table PUD/PMD descriptor
 *                        (block entry) is encountered.
 *   AARCH64_PT_ENOMEM    when a 4 KiB table page cannot be allocated.
 *
 * The root is a high-half direct-map pointer. Active-root callers may
 * invoke this only before smp_boot_aps() — see the BSP-pre-SMP note in
 * the file header. */
int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                      uint32_t perm);

/* Walk the tree and report the leaf at VA without allocating. Returns
 * ENOENT for any absent level (PGD/PUD/PMD/PTE) and ECONFLICT for a
 * valid PUD/PMD block descriptor (this layer only owns 4 KiB leaves).
 * On OK, `*pa_out` and `*perm_out` receive the decoded physical base
 * (page-aligned) and permission word respectively.
 *
 * `root` is a high-half direct-map pointer. */
int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out);

/* Clear an existing 4 KiB leaf. Returns the prior physical address and
 * permission via `*pa_out` / `*perm_out` before the clear. Same error
 * contract as query_4k; never allocates. If the path is absent (ENOENT)
 * or blocked (ECONFLICT), the tree is not modified. */
int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                        uint64_t *pa_out, uint32_t *perm_out);

/* Return true iff every page in [va, va + length) is mapped with the
 * requested access. length == 0 returns true. addr + length overflow
 * returns false. Requests a writable mapping when `write` is true and
 * a user-accessible mapping when `user` is true. Never allocates.
 *
 * In this increment the layer only owns 4 KiB leaves; any PUD/PMD
 * block entry seen on the path returns false (conservative). */
bool aarch64_pt_range_accessible(const uint64_t *root, uint64_t va,
                                uint64_t length, bool write, bool user);

#endif /* OS01_AARCH64_PAGE_TABLE_H */