/* kernel/arch/aarch64/ram_core.c
 *
 * Linkable empty core for the AArch64 UEFI RAM map pipeline. Task 1
 * only ships this file so the host contract test (see
 * tests/aarch64_ram_test.py) can prove that the declared signatures
 * in `ram_core.h` resolve against a real translation unit.
 *
 * Both functions return a non-zero error (`-EIO`) and, when given a
 * non-NULL output buffer, leave it zeroed. The real normalizer
 * arrives in Task 2; the real publisher in Task 3. Until then,
 * callers see the contract is linkable, the headers are owned by
 * `kernel/arch/aarch64/ram_core.h`, and the stubs make no
 * architectural promise.
 *
 * Host-linkable AND freestanding-target-linkable: this file avoids
 * `<string.h>` and `<stdlib.h>` (no libc available when the kernel
 * links it) so the same translation unit compiles both under
 * `cc -std=c11` for the contract test and under the
 * `aarch64-none-elf` clang for the kernel build. The only headers
 * used are `<errno.h>`, `<stddef.h>`, and `<stdint.h>`.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* ram_core.h is kernel-internal (kernel/arch/aarch64/), not in
 * kernel/include/, so the kernel's `-Iinclude` search path does not
 * find it under `<kernel/arch/aarch64/ram_core.h>`. Include it
 * relative to this file (matching the style of boot_percpu.c, smp.c,
 * and the other aarch64 C files). The header itself pulls in
 * kernel/include/kernel/{bootinfo.h,arch/aarch64/ram.h}. */
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

int aarch64_ram_normalize(const struct boot_context *ctx,
                          struct aarch64_ram_map *out)
{
    (void)ctx;
    zero_bytes(out, sizeof(*out));
    return -EIO;
}

int aarch64_ram_publish_once(const struct aarch64_ram_map *candidate,
                             struct aarch64_ram_map *destination,
                             int *initialized)
{
    (void)candidate;
    zero_bytes(destination, sizeof(*destination));
    if (initialized)
        *initialized = 0;
    return -EIO;
}
