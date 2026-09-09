/* kernel/memory/pmm_arch.c — weak default dispatcher.
 *
 * The default pmm_arch_normalize returns 0 (fatal-no-ranges); the
 * default pmm_arch_zone_split returns SIZE_MAX (no unmapped zones).
 * These defaults contain no architecture-specific symbols, so they
 * link cleanly on every architecture. Each architecture provides
 * its strong override in kernel/arch/<arch>/pmm_arch.c.
 */

#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>

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
