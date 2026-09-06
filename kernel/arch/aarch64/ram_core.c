/* kernel/arch/aarch64/ram_core.c
 *
 * Linkable empty core for the AArch64 UEFI RAM map pipeline. Task 1
 * only ships this file so the host contract test (see
 * tests/aarch64_ram_test.py) can prove that the declared signatures
 * in `ram_core.h` resolve against a real translation unit.
 *
 * Both functions return a non-zero negative error code and, when
 * given a non-NULL output buffer, leave it zeroed. The real
 * normalizer arrives in Task 2; the real publisher in Task 3. Until
 * then, callers see the contract is linkable, the headers are owned
 * by `kernel/arch/aarch64/ram_core.h`, and the stubs make no
 * architectural promise.
 *
 * Host-linkable AND freestanding-target-linkable: this file uses
 * only `<stddef.h>` and `<stdint.h>` (no `<errno.h>`, no `<string.h>`,
 * no `<stdlib.h>`) so the same translation unit compiles both under
 * `cc -std=c11` for the contract test and under the
 * `aarch64-none-elf` clang for the kernel build. Error codes are
 * literal small negatives — see ram_core.h for the contract.
 */
#include <stddef.h>
#include <stdint.h>

/* ram_core.h is kernel-internal (kernel/arch/aarch64/), not in
 * kernel/include/, so the kernel's `-Iinclude` search path does not
 * find it under `<kernel/arch/aarch64/ram_core.h>`. Include it
 * relative to this file (matching the style of boot_percpu.c, smp.c,
 * and the other aarch64 C files). The header itself pulls in
 * kernel/include/kernel/arch/aarch64/ram.h. */
#include "ram_core.h"

static void zero_bytes(void *buffer, size_t size)
{
    volatile uint8_t *cursor = (volatile uint8_t *)buffer;
    size_t index;

    if (!buffer)
        return;
    for (index = 0; index < size; ++index)
        cursor[index] = 0;
}

int aarch64_ram_normalize(const uint8_t *bytes, uint32_t entry_count,
                          uint32_t entry_size, uint32_t format,
                          uint32_t descriptor_version,
                          const struct aarch64_ram_interval *exclude,
                          uint32_t exclude_count,
                          struct aarch64_ram_map *out)
{
    (void)bytes;
    (void)entry_count;
    (void)entry_size;
    (void)format;
    (void)descriptor_version;
    (void)exclude;
    (void)exclude_count;
    zero_bytes(out, sizeof(*out));
    return -1;
}

int aarch64_ram_publish_once(const struct aarch64_ram_map *candidate,
                             struct aarch64_ram_map *destination,
                             int *initialized)
{
    (void)candidate;
    zero_bytes(destination, sizeof(*destination));
    if (initialized)
        *initialized = 0;
    return -1;
}
