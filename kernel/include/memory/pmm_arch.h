#ifndef _KERNEL_PMM_ARCH_H
#define _KERNEL_PMM_ARCH_H

/*
 * kernel/include/memory/pmm_arch.h — PMM arch-facade for boot reservation
 * strategy + range-based reservation helper + single-frame exact claim
 * (aarch64 M1 plan Task 2).
 *
 * The legacy x86_64 boot loop reserved every represented frame whose
 * RAM-relative index fell below the kernel-metadata end. That forced
 * all boot metadata to live below the lowest RAM frame (always true
 * for UEFI on x86_64, never true for aarch64 with DRAM at 0x40000000+).
 *
 * Task 2 splits the responsibility:
 *
 *   - pmm_arch_boot_reservations(strategy) returns the set of PA ranges
 *     that must be reserved during pmm_init. The default weak impl in
 *     kernel/memory/pmm_arch.c returns the legacy prefix [0,
 *     ceil2M(metadata_end_pa)). aarch64 (Task 3) overrides this with a
 *     strong symbol that returns two ranges: a low prefix and the
 *     arena window inside its representative zone.
 *
 *   - pmm_reserve_boot_ranges(helper) walks pages_struct once, marks
 *     every represented frame whose PA falls in the given ranges, and
 *     does the same per-frame bookkeeping the legacy loop performed
 *     (flip bit, page_init with PG_PTable_Mapped|Kernel_Init|Kernel,
 *     using/free/total_pages_link counters). Idempotent: a second call
 *     with the same ranges produces no change.
 *
 *   - pmm_claim_free_frame is a single-frame allocator that picks the
 *     lowest (from_end=false) or highest (from_end=true) free frame
 *     inside [start_pa, end_pa). It uses the same bitmap / attribute /
 *     counter conventions as alloc_pages(ZONE_NORMAL, 1) and is held
 *     under pmm_lock. Holes (zone_struct == NULL) are skipped — claim
 *     never returns a Page whose PA is not in represented RAM.
 *
 * The caller of claim, if it needs page_init refcount tracking, calls
 * page_init separately and pairs it with page_clean → free_pages when
 * releasing the frame. claim itself does NOT call page_init; doing so
 * would double-count total_pages_link.
 *
 * struct pmm_layout carries the metadata_end_pa field needed by the
 * default strategy. The calculator (kernel/memory/pmm_boot.c) does not
 * populate it — the caller of pmm_layout_calculate (pmm.c) sets it
 * after computing Virt_To_Phy(brk + layout.end_of_struct_off).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <memory/pmm.h>
#include <memory/pmm_boot.h>

struct pmm_phys_range {
    uint64_t start;   /* inclusive, granule-aligned PA */
    uint64_t end;     /* exclusive, granule-aligned PA */
};

/* Strategy: enumerate the PA ranges the boot must reserve. The
 * capacity/count pattern lets a caller query the count first
 * (capacity == 0, out may be NULL) and allocate afterwards.
 *
 * Returns 0 with *count set; -EINVAL on NULL layout or NULL count;
 * the weak default never returns -EOVERFLOW. Strong overrides may
 * report more detailed errors. */
int pmm_arch_boot_reservations(const struct pmm_layout *layout,
                               struct pmm_phys_range *out,
                               size_t capacity,
                               size_t *count);

/* Walk pages_struct; for every represented frame whose PA falls in
 * any range, mark it reserved (flip bit, page_init with the kernel
 * metadata attribute set, update using/free/total_pages_link).
 * Idempotent: re-reserving an already-reserved frame is a no-op.
 *
 * Returns 0 on success; -EINVAL if pm is NULL, count > 0 and ranges is
 * NULL, any range has start > end, or any range endpoint is not
 * 2 MiB-aligned. */
int pmm_reserve_boot_ranges(struct Physical_Memory_Manager *pm,
                            const struct pmm_phys_range *ranges,
                            size_t count);

/* In pmm_lock, find a single free frame inside [start_pa, end_pa)
 * and claim it (flip bit, set attribute = PG_PTable_Mapped, update
 * using/free counters). from_end selects the lowest (false) or highest
 * (true) free frame in the range. Returns NULL if end <= start, the
 * range contains no free represented frame, or the range falls
 * entirely in a hole.
 *
 * claim does NOT call page_init — the caller pairs page_clean →
 * free_pages when releasing. */
struct Page *pmm_claim_free_frame(uint64_t start_pa, uint64_t end_pa,
                                  bool from_end);

#endif /* _KERNEL_PMM_ARCH_H */
