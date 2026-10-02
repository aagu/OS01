/* kernel/memory/pmm_arch.c — weak default dispatcher.
 *
 * The default pmm_arch_normalize returns 0 (fatal-no-ranges); the
 * default pmm_arch_zone_split returns SIZE_MAX (no unmapped zones).
 * The default pmm_arch_boot_reservations returns the legacy x86_64
 * prefix [0, ceil2M(metadata_end_pa)).
 *
 * These defaults contain no architecture-specific symbols, so they
 * link cleanly on every architecture. Each architecture provides
 * its strong override in kernel/arch/<arch>/pmm_arch.c (aarch64 will
 * override pmm_arch_boot_reservations to add an arena range inside
 * its representative zone).
 */

#include <errno.h>

#include <core/bootinfo.h>
#include <memory/memory_map.h>
#include <memory/pmm_arch.h>

__attribute__((weak))
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;
    (void)out;
    return 0;   /* fatal: caller sees 0 and halts via [smp] FATAL */
}

__attribute__((weak))
uint64_t pmm_arch_zone_split(void)
{
    return SIZE_MAX;
}

/* Default weak strategy: legacy x86_64 prefix reservation. The
 * aarch64 strong override (kernel/arch/aarch64/pmm_arch.c) will
 * return multiple ranges covering the low metadata prefix plus an
 * arena window in the representative zone. */
__attribute__((weak))
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
