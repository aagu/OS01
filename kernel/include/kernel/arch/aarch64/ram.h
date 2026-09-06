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
 * described by the constants at the top of this file. Those four
 * constants are pinned by the host contract test (see
 * tests/aarch64_ram_test.py); any drift fails the build.
 *
 * `aarch64_ram_map` is the public target the kernel's early MMU code
 * will iterate; the internal `struct aarch64_ram_interval` lives in
 * ram_core.h next to the normalizer's signature.
 */
#ifndef OS01_AARCH64_RAM_H
#define OS01_AARCH64_RAM_H

#include <stdint.h>

/* The raw UEFI memory descriptor the firmware emits is 48 bytes; the
 * loader prepends a 32-byte prefix (a `boot_memory_map` head that
 * already lives inside `boot_context`) before the descriptor array.
 * `AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE` lets the kernel skip that
 * prefix without re-parsing the layout every boot. */
#define AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE  32u

/* UEFI memory type 7 = EfiConventionalMemory, the only descriptor
 * type the normalizer retains. Everything else (MMIO, reserved,
 * ACPI, runtime services) is mapped by the loader or firmware and
 * must NOT be handed to the kernel's general-purpose page pool. */
#define AARCH64_EFI_CONVENTIONAL_MEMORY      7u

/* The normalizer rounds every retained interval to this granule so
 * the kernel can map each range with section descriptors in the
 * early page tables. 2 MiB also matches the largest block the
 * page-table walker recognises on the bring-up path. */
#define AARCH64_RAM_GRANULE                  (UINT64_C(1) << 21)

/* Fixed map capacity: 16 ranges cover the AArch64 virt platform's
 * practical RAM distribution (a few low DRAM blocks plus the high
 * DMA window) without forcing the kernel to carry a dynamic vector.
 * If the loader ever produces more, the normalizer must coalesce
 * before storing; the contract test pins the capacity so any
 * enlargement here also touches the storage layout. */
#define AARCH64_RAM_MAX_RANGES               16

struct aarch64_ram_interval {
    uint64_t start;
    uint64_t end;
};

struct aarch64_ram_map {
    uint32_t count;
    struct aarch64_ram_interval ranges[AARCH64_RAM_MAX_RANGES];
};

#endif /* OS01_AARCH64_RAM_H */
