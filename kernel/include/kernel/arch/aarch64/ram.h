/* kernel/include/kernel/arch/aarch64/ram.h
 *
 * Public AArch64 RAM map produced by the UEFI normalizer. The map is
 * a fixed-capacity array of `[start, end)` intervals expressed in
 * 2 MiB-aligned physical addresses (the granule matches the
 * section-mapped pages the kernel will use to bring the rest of
 * memory up after `boot_context`).
 *
 * The wire format the loader hands the kernel — the raw UEFI memory
 * descriptors with a 32-byte prefix the kernel needs to skip — is
 * described by the wire constants in `ram_core.h`. The capacity
 * constant is pinned by the host contract test (see
 * tests/aarch64_ram_test.py); any drift fails the build.
 *
 * `aarch64_ram_map` is the public target the kernel's early MMU code
 * will iterate; the internal `struct aarch64_ram_interval` lives in
 * `ram_core.h` next to the normalizer's signature and is reserved
 * for the exclusion list passed into the pure normalizer.
 */
#ifndef OS01_AARCH64_RAM_H
#define OS01_AARCH64_RAM_H

#include <stdint.h>

/* Fixed map capacity: 16 ranges cover the AArch64 virt platform's
 * practical RAM distribution (a few low DRAM blocks plus the high
 * DMA window) without forcing the kernel to carry a dynamic vector.
 * If the loader ever produces more, the normalizer must coalesce
 * before storing; the contract test pins the capacity so any
 * enlargement here also touches the storage layout. */
#define AARCH64_RAM_MAX_RANGES               16

struct aarch64_ram_range {
    uint64_t start;  /* inclusive physical address, 2 MiB aligned */
    uint64_t end;    /* exclusive physical address, 2 MiB aligned */
};

struct aarch64_ram_map {
    uint32_t count;
    struct aarch64_ram_range ranges[AARCH64_RAM_MAX_RANGES];
};

/* Forward declaration of the handoff layout owned by
 * <kernel/bootinfo.h>. The full type is not required here because the
 * public surface only takes its pointer; downstream translation units
 * that need the fields can include bootinfo.h themselves. */
struct boot_context;

/* Initialize the published RAM map from the UEFI handoff. Returns 0
 * only when it has published a complete map; every negative return
 * is fatal to the boot caller (the BSP halts before GIC, SMP, or
 * timer setup). The definitions live in `ram.c` and are owned by
 * Task 3 of the AArch64 UEFI RAM normalization plan; this header
 * only publishes the contract so callers may compile against it
 * without waiting on Task 3. */
int aarch64_ram_init(const struct boot_context *handoff);

/* Return a non-null pointer to the published, immutable RAM map
 * after a successful aarch64_ram_init() call. Callers must treat
 * the returned object as read-only; the pointer is null until
 * initialization completes. */
const struct aarch64_ram_map *aarch64_ram_map_get(void);

#endif /* OS01_AARCH64_RAM_H */
