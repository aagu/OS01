/* hosttests/mock/pmm_arch_fixture.c — host fixture stubs for the PMM
 * arch strategy hooks (mm-architecture-boundaries Task 4).
 *
 * The production hooks now exist ONLY as per-arch strong definitions
 * (no weak default).  A host fixture that does not exercise a given
 * input still needs the symbol to link, so this file is compiled
 * TWICE with disjoint macros:
 *
 *   -DPMM_FIXTURE_NORMALIZE  -> pmm_arch_normalize returns 0
 *                               (fatal-no-ranges)
 *   -DPMM_FIXTURE_ZONE_SPLIT -> pmm_arch_zone_split returns UINT64_MAX
 *                               (no unmapped-zone split)
 *
 * The stubs only supply inputs the fixture under test does not
 * exercise — they never replace a production reservation strategy:
 * fixtures that DO test reservations link the production x86
 * pmm_reserve.c (real legacy prefix) or the production ARM pmm_arch.c
 * + early_arena.c reservation instead.
 */

#include <stddef.h>
#include <stdint.h>

#include <core/bootinfo.h>
#include <memory/memory_map.h>

#ifdef PMM_FIXTURE_NORMALIZE
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;
    (void)out;
    return 0;   /* fatal: caller sees 0 and halts via [smp] FATAL */
}
#endif

#ifdef PMM_FIXTURE_ZONE_SPLIT
uint64_t pmm_arch_zone_split(void)
{
    return UINT64_MAX;
}
#endif
