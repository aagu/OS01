#ifndef OS01_ARCH_BOOT_MEMORY_H
#define OS01_ARCH_BOOT_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <memory/memory_map.h>
/* PMM initialized, BSP before AP startup. Once per boot, including failure.
 * Success means mapping and local TLB maintenance were verified. */
int arch_boot_direct_map_init(void);
bool arch_boot_direct_map_ready(void);
/* Output-only count. NULL arguments: -EINVAL without writes. Valid arguments
 * first become NULL/0; not ready: -EAGAIN. Success returns immutable merged
 * mapped RAM coverage, not the free-list, held until boot ends. */
int arch_boot_direct_map_ranges(const struct MEMORY_RANGE **out,size_t *count);
#endif
