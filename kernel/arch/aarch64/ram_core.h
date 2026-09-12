/* kernel/arch/aarch64/ram_core.h
 *
 * Internal contract layer for the AArch64 UEFI RAM map pipeline.
 *
 * Two production symbols live here, consumed by `boot/uefi`'s
 * loader and by the kernel's early MMU setup:
 *
 *   - `aarch64_ram_normalize()` — reads the raw UEFI descriptor
 *     bytes and produces a deduplicated, granule-aligned,
 *     conventional-memory-only `aarch64_ram_map`.
 *
 *   - `aarch64_ram_publish_once()` — guards the kernel-side copy
 *     against double publication; the loader uses it to install
 *     the map exactly once even if a misbehaving caller retries.
 *
 * Task 1 only proves the contract: the matching `ram_core.c` ships
 * a linkable empty core (stub) that returns a non-zero error. Task
 * 2 implements the normalizer; Task 3 implements the publisher. The
 * signatures here are stable from this commit onward.
 *
 * Host-linkable: the implementation depends only on `<stdint.h>`
 * and `<stddef.h>` so the same .c file compiles under cc on a
 * developer workstation and under the aarch64-none-elf clang at
 * build time. No kernel-internal headers; no libc.
 */
#ifndef OS01_AARCH64_RAM_CORE_H
#define OS01_AARCH64_RAM_CORE_H

#include <stdint.h>
#include <stddef.h>

#include <arch/aarch64/ram.h>

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

/* Maximum number of exclusion intervals the normalizer accepts in a
 * single call. The internal `emit_aligned_fragments()` keeps a fixed
 * stack scratch array sized to this cap; rejecting the call here keeps
 * the spec's O(1)-scratch contract honest by failing closed instead of
 * silently truncating overflow exclusions. */
#define RAM_FRAG_SCRATCH_MAX                 8u

/* Distinct error codes for `aarch64_ram_normalize()` and
 * `aarch64_ram_publish_once()`. Each maps to one concrete failure
 * mode so the boot caller can log the right reason. They are
 * negative small literals (no `<errno.h>`) so the kernel and the
 * host runner share the same wire values. */
#define AARCH64_RAM_OK                       0
#define AARCH64_RAM_ERR_ARGUMENT            (-1)
#define AARCH64_RAM_ERR_GEOMETRY            (-2)
#define AARCH64_RAM_ERR_FORMAT              (-3)
#define AARCH64_RAM_ERR_VERSION             (-4)
#define AARCH64_RAM_ERR_OVERFLOW            (-5)
#define AARCH64_RAM_ERR_CAPACITY            (-6)

/* Closed-open `[start, end)` physical interval used by the
 * normalizer's exclusion list. Reserved for the internal contract
 * layer — the public range type the kernel iterates is
 * `struct aarch64_ram_range` in <arch/aarch64/ram.h>. */
struct aarch64_ram_interval {
    uint64_t start;
    uint64_t end;
};

/* Normalize the raw UEFI memory map into a deduplicated,
 * granule-aligned, conventional-memory-only `aarch64_ram_map`.
 *
 * The function is pure: allocation-free, no static state, no host
 * environment dependencies. The boot wrapper validates the handoff,
 * converts the descriptor physical address through the direct map,
 * and calls this function with the resulting host pointer. The host
 * runner passes synthetic descriptor bytes directly.
 *
 * Parameters:
 *   - `bytes` points to `entry_count * entry_size` bytes of raw
 *     descriptor data. The caller must have established
 *     byte-range accessibility before calling.
 *   - `entry_size` must be at least
 *     `AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE` (32).
 *   - `format` must equal `BOOT_MEMORY_FORMAT_UEFI_RAW`.
 *   - `descriptor_version` must equal 1.
 *   - `exclude` / `exclude_count` give a closed-open list of
 *     physical intervals (kernel image, handoff allocation, ...) to
 *     subtract from every candidate before alignment.
 *
 * On success writes the result into `*out` (when non-NULL) and
 * returns 0. On any failure — zero `entry_count`, `entry_size`
 * below 32, unsupported format or version, null pointer, invalid
 * exclusion (`end <= start`), arithmetic overflow, or more than
 * `AARCH64_RAM_MAX_RANGES` surviving ranges — leaves `*out` in a
 * defined empty state and returns a distinct negative error code.
 * The caller MUST treat a non-zero return as fatal; the normalizer
 * never produces a partial map. */
int aarch64_ram_normalize(const uint8_t *bytes, uint32_t entry_count,
                          uint32_t entry_size, uint32_t format,
                          uint32_t descriptor_version,
                          const struct aarch64_ram_interval *exclude,
                          uint32_t exclude_count,
                          struct aarch64_ram_map *out);

/* Publish `*candidate` into `*destination` exactly once across the
 * loader's lifetime.
 *
 * On the first successful call `*destination` is overwritten with
 * `*candidate` and `*initialized` is set to a non-zero value. Any
 * subsequent call — regardless of `*candidate`'s contents — returns
 * `-2` without touching `*destination`, so a buggy retry cannot
 * clobber the map the kernel already accepted.
 *
 * Before copying, the helper validates that candidate `count` is in
 * `1..AARCH64_RAM_MAX_RANGES` and every range is non-empty, 2 MiB
 * aligned, strictly ordered, and separated from its neighbours;
 * otherwise it returns a negative error code and changes neither
 * state nor destination.
 *
 * The flag is an `int *` (not `bool`) to keep the signature
 * portable across the loader (LLP64) and the kernel (LP64). */
int aarch64_ram_publish_once(const struct aarch64_ram_map *candidate,
                             struct aarch64_ram_map *destination,
                             int *initialized);

#endif /* OS01_AARCH64_RAM_CORE_H */
