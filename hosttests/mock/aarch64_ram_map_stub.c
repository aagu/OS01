/* hosttests/mock/aarch64_ram_map_stub.c — NULL map stub for host
 * fixtures that link the production ARM pmm_arch.c (normalize + zone
 * split) without publishing a RAM map (mm-architecture-boundaries
 * Task 4).  aarch64_ram_map_get() == NULL makes the production
 * normalize return 0 — the documented "caller violated ordering"
 * path; the stub only supplies the unpublished input, never a
 * reservation strategy. */

#include <stddef.h>
#include <arch/aarch64/ram.h>

const struct aarch64_ram_map *aarch64_ram_map_get(void)
{
    return NULL;
}
