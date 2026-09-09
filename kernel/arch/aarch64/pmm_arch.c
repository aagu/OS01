/* kernel/arch/aarch64/pmm_arch.c — strong override for aarch64.
 *
 * Reads the already-published aarch64_ram_map (single source of truth;
 * aarch64_ram_init published it before pmm_init is called). Each emitted
 * range becomes a MEMORY_TYPE_RAM MEMORY_RANGE. The kernel-LMA + handoff
 * excludes are already baked into the published map by aarch64_ram_init.
 *
 * Returns 0 if aarch64_ram_map_get() returns NULL — caller violated
 * ordering (aarch64_ram_init must be invoked before pmm_init).
 */

#include <stddef.h>
#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>
#include <kernel/arch/aarch64/ram.h>

size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;
    const struct aarch64_ram_map *m = aarch64_ram_map_get();
    if (m == NULL) return 0;
    if (m->count == 0 || m->count > MEMORY_RANGE_MAX) return 0;
    for (size_t i = 0; i < m->count; i++) {
        out[i].phys_start = m->ranges[i].start;
        out[i].phys_end   = m->ranges[i].end;
        out[i].type       = MEMORY_TYPE_RAM;
    }
    return m->count;
}
