/* kernel/arch/aarch64/ram_core.h
 *
 * Internal contract layer for the AArch64 UEFI RAM map pipeline.
 *
 * Two production symbols live here, consumed by `boot/uefi`'s
 * loader and by the kernel's early MMU setup:
 *
 *   - `aarch64_ram_normalize()` — reads the raw UEFI descriptors
 *     referenced by `boot_context` and produces a deduplicated,
 *     granule-aligned, conventional-memory-only `aarch64_ram_map`.
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
 * Host-linkable: the implementation depends only on `<stdint.h>`,
 * `<stddef.h>`, `<errno.h>`, and `<string.h>` so the same .c file
 * compiles under cc on a developer workstation and under the
 * aarch64-none-elf clang at build time. No kernel-internal headers.
 */
#ifndef OS01_AARCH64_RAM_CORE_H
#define OS01_AARCH64_RAM_CORE_H

#include <stdint.h>
#include <stddef.h>

#include <kernel/bootinfo.h>
#include <kernel/arch/aarch64/ram.h>

/* Normalize the raw UEFI memory map referenced by `boot_context`
 * into a deduplicated, granule-aligned, conventional-memory-only
 * `aarch64_ram_map`.
 *
 * On success writes the result into `*out` (when non-NULL) and
 * returns 0. On any failure — NULL pointer, `out` is NULL,
 * descriptor count overflow, version mismatch, too many surviving
 * intervals for `AARCH64_RAM_MAX_RANGES` — leaves `*out` in a
 * defined empty state and returns a negative errno-like code. The
 * caller MUST treat a non-zero return as fatal; the normalizer
 * never produces a partial map. */
int aarch64_ram_normalize(const struct boot_context *ctx,
                          struct aarch64_ram_map *out);

/* Publish `*candidate` into `*destination` exactly once across the
 * loader's lifetime.
 *
 * On the first successful call `*destination` is overwritten with
 * `*candidate` and `*initialized` is set to a non-zero value. Any
 * subsequent call — regardless of `*candidate`'s contents — returns
 * `-EALREADY` without touching `*destination`, so a buggy retry
 * cannot clobber the map the kernel already accepted.
 *
 * The flag is an `int *` (not `bool`) to keep the signature
 * portable across the loader (LLP64) and the kernel (LP64). */
int aarch64_ram_publish_once(const struct aarch64_ram_map *candidate,
                             struct aarch64_ram_map *destination,
                             int *initialized);

#endif /* OS01_AARCH64_RAM_CORE_H */
