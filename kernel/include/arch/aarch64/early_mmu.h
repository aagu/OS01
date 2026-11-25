#ifndef OS01_AARCH64_EARLY_MMU_H
#define OS01_AARCH64_EARLY_MMU_H

#include <stdint.h>

/* device_base/device_end: the early Device window (2 MiB-aligned),
 * from the parked boot_context handoff or the compile-time QEMU
 * constants (head.S resolves the fallback). */
void aarch64_build_boot_page_tables(uint64_t zeroed_tables[6][512],
                                    uint64_t boot_base,
                                    uint64_t device_base,
                                    uint64_t device_end);

/* Add one more Device window to the tables the builder above laid out:
 * the Device window itself when it sits above the identity map, or the
 * console's 2 MiB block when it lies outside that window. `table` is one
 * of the two reserved L2 pages (4 or 5) and only matters for a window
 * above [0, AARCH64_BOOT_MAP_END): it is hung off the window's 1 GiB L1
 * slot, where blocks outside the window stay invalid; a second window in
 * the same slot extends the table already there. head.S validates the
 * window's shape before calling. */
void aarch64_build_boot_window(uint64_t zeroed_tables[6][512], unsigned table,
                               uint64_t base, uint64_t end);

#endif
