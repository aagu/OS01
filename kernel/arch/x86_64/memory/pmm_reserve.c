/* kernel/arch/x86_64/memory/pmm_reserve.c — strong x86 boot reservation
 * strategy (mm-architecture-boundaries Task 4).
 *
 * Moved verbatim from the former weak default in kernel/memory/
 * pmm_arch.c: the legacy x86_64 prefix reservation [0,
 * ceil2M(metadata_end_pa)).  This TU is independent of the E820
 * normalize/zone-split strategy (kernel/arch/x86_64/memory/pmm_arch.c)
 * so synthetic-RAM host fixtures can link the REAL reservation without
 * linking E820 normalize.
 *
 * Every architecture now defines all three pmm_arch_* strategy hooks
 * strongly; there is no weak default any more — a port missing one
 * fails at link time instead of silently using another port's prefix.
 */

#include <errno.h>

#include <memory/memory_map.h>
#include <memory/pmm_arch.h>

int pmm_arch_boot_reservations(const struct pmm_layout *layout,
                               struct pmm_phys_range *out,
                               size_t capacity,
                               size_t *count)
{
    if (!layout || !count) return -EINVAL;
    *count = 1;
    if (capacity == 0) return 0;   /* query mode */
    if (!out) return -EINVAL;
    /* Ceil PA to next 2 MiB so the metadata's tail page is fully
     * covered. (PMMngr.end_of_struct's trailing slack is already
     * included by pmm_layout_calculate's total_bytes, but the legacy
     * x86_64 loop rounded up to 2 MiB anyway — keep parity.) */
    uint64_t end = (layout->metadata_end_pa + PAGE_2M_SIZE - 1)
                   & ~(PAGE_2M_SIZE - 1);
    out[0].start = 0;
    out[0].end   = end;
    return 0;
}
