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

#endif
