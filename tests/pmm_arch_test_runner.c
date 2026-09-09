/* tests/pmm_arch_test_runner.c — host-side test for pmm_arch. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>

extern size_t pmm_arch_normalize(const struct boot_context *,
                                  struct MEMORY_RANGE *);
extern uint64_t pmm_arch_zone_split(void);

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    return 1; } } while (0)

int main(void)
{
    struct MEMORY_RANGE out[MEMORY_RANGE_MAX];

#if !defined(__aarch64__)
    /* x86_64-only: aarch64 adapter ignores ctx and reads the
     * already-published map (aarch64_ram_map_get), so a zeroed
     * boot_context has no defined meaning on aarch64. */
    {
        struct boot_context ctx = {0};
        CHECK(pmm_arch_normalize(&ctx, out) == 0);
    }

    /* Minimal E820 fixture exercising the host smoke test. The
     * on-target multi-fragment verification happens via
     * `make test-aarch64-uefi-smp` (see pmm_arch implementation
     * plan: tests/aarch64_uefi_smp.py). The host runner only
     * verifies: (a) `n >= 1` (at least one RAM range survives),
     * (b) `phys_end > phys_start`, (c) granule alignment,
     * (d) type == MEMORY_TYPE_RAM. */
    {
        struct E820_ENTRY e[] = { { .address = 0, .length = 0x40000000,
                                     .type = 1 } };
        struct boot_context ctx = { .magic = BOOT_CONTEXT_MAGIC,
                                     .version = BOOT_CONTEXT_VERSION,
                                     .size = sizeof(ctx),
                                     .flags = BOOT_CONTEXT_HAS_MEMORY_MAP,
                                     .memory = { .entries = (uintptr_t)e,
                                                 .entry_count = 1,
                                                 .entry_size = sizeof(struct E820_ENTRY),
                                                 .format = BOOT_MEMORY_FORMAT_E820 } };
        size_t n = pmm_arch_normalize(&ctx, out);
        CHECK(n >= 1);
        for (size_t i = 0; i < n; i++) {
            CHECK(out[i].phys_end > out[i].phys_start);
            CHECK((out[i].phys_start & (MEMORY_RANGE_GRANULE - 1)) == 0);
            CHECK((out[i].phys_end & (MEMORY_RANGE_GRANULE - 1)) == 0);
            CHECK(out[i].type == MEMORY_TYPE_RAM);
        }
    }
#endif

#if defined(__x86_64__)
    CHECK(pmm_arch_zone_split() == 0x100000000ULL);
#elif defined(__aarch64__)
    CHECK(pmm_arch_zone_split() == SIZE_MAX);
#endif

    return 0;
}