/* qemutests/arch_runner/pmm_arch_test_runner.c — host-side test for pmm_arch. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <core/bootinfo.h>
#include <memory/memory_map.h>
#include <arch/x86_64/bootinfo_x86.h>   /* struct E820_ENTRY, BOOT_MEMORY_FORMAT_E820 */

extern size_t pmm_arch_normalize(const struct boot_context *,
                                  struct MEMORY_RANGE *);
extern uint64_t pmm_arch_zone_split(void);

/* Runner-specific architecture selector (mm-architecture-boundaries
 * Task 4): the driver defines OS01_PMM_ARCH_TEST_ARM=1 when it links
 * the production ARM pmm_arch.c, and leaves it undefined for the x86
 * runner.  Compiler architecture macros are NOT faked — the ARM
 * strategy hooks are plain C and run natively on the host. */
#if defined(OS01_PMM_ARCH_TEST_ARM)
#define PMM_ARCH_TEST_ARM 1
#else
#define PMM_ARCH_TEST_ARM 0
#endif

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    return 1; } } while (0)

int main(void)
{
    struct MEMORY_RANGE out[MEMORY_RANGE_MAX];

    /* Regression (32-bit truncation): MEMORY_RANGE_GRANULE must be a
     * 64-bit constant. With `1u << 21`, `~(MEMORY_RANGE_GRANULE - 1)`
     * evaluates in 32-bit arithmetic (0xFFE00000) and zero-extends,
     * so masking an address above 4 GiB clears its high bits. This
     * is exactly the expression pmm_init Step 2/4 uses to compute
     * lowest_ram and the granule-aligned zone bounds. */
    {
        uint64_t above_4g = 0x100000000ULL;   /* 4 GiB, granule-aligned */
        uint64_t masked = above_4g & ~(MEMORY_RANGE_GRANULE - 1);
        CHECK(masked == 0x100000000ULL);
        CHECK(((uint64_t)~(MEMORY_RANGE_GRANULE - 1)) ==
              0xFFFFFFFFFFE00000ULL);
    }

#if !PMM_ARCH_TEST_ARM
    /* x86 runner: the E820 adapter reads ctx. */
    {
        struct boot_context ctx = {0};
        CHECK(pmm_arch_normalize(&ctx, out) == 0);
    }

    /* Minimal E820 fixture exercising the host smoke test. The
     * on-target multi-fragment verification happens via
     * `make test-aarch64-uefi-smp` (see pmm_arch implementation
     * plan: qemutests/aarch64_uefi_smp.py). The host runner only
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

    /* Regression (type preservation): surviving fragments must keep
     * the E820-derived type instead of being forced to
     * MEMORY_TYPE_RAM. pmm_init Step 2/4 only walks MEMORY_TYPE_RAM
     * ranges, so relabeling reserved/ACPI ranges as RAM silently
     * feeds MMIO/reserved physical memory into the allocator's
     * domain. Also: adjacent ranges of DIFFERENT types must never
     * merge (the merge step may only collapse same-type neighbors).
     * All fragments here sit above the kernel-LMA/handoff/trampoline
     * excludes, so exactly three ranges must survive. */
    {
        struct E820_ENTRY e[] = {
            { .address = 0x80000000ULL, .length = 0x200000ULL, .type = 1 },
            { .address = 0x80200000ULL, .length = 0x200000ULL, .type = 2 },
            { .address = 0x90000000ULL, .length = 0x200000ULL, .type = 3 },
        };
        struct boot_context ctx = { .magic = BOOT_CONTEXT_MAGIC,
                                     .version = BOOT_CONTEXT_VERSION,
                                     .size = sizeof(ctx),
                                     .flags = BOOT_CONTEXT_HAS_MEMORY_MAP,
                                     .memory = { .entries = (uintptr_t)e,
                                                 .entry_count = 3,
                                                 .entry_size = sizeof(struct E820_ENTRY),
                                                 .format = BOOT_MEMORY_FORMAT_E820 } };
        size_t n = pmm_arch_normalize(&ctx, out);
        CHECK(n == 3);
        CHECK(out[0].phys_start == 0x80000000ULL);
        CHECK(out[0].phys_end   == 0x80200000ULL);   /* not merged through reserved */
        CHECK(out[0].type       == MEMORY_TYPE_RAM);
        CHECK(out[1].phys_start == 0x80200000ULL);
        CHECK(out[1].phys_end   == 0x80400000ULL);
        CHECK(out[1].type       == MEMORY_TYPE_RESERVED);
        CHECK(out[2].phys_start == 0x90000000ULL);
        CHECK(out[2].phys_end   == 0x90200000ULL);
        CHECK(out[2].type       == MEMORY_TYPE_ACPI_RECLAIM);
    }
#endif

#if PMM_ARCH_TEST_ARM
    /* ARM runner: the production strategy was linked with an
     * UNPUBLISHED RAM map (the stub returns NULL), so the documented
     * ordering-violation path is normalize == 0, and the zone
     * threshold is explicitly "none" — UINT64_MAX, never the x86 4
     * GiB boundary (weak-default regression: the old weak default
     * returned SIZE_MAX via a weak symbol that no longer exists). */
    CHECK(pmm_arch_normalize(NULL, out) == 0);
    CHECK(pmm_arch_zone_split() == UINT64_MAX);
#else
    CHECK(pmm_arch_zone_split() == 0x100000000ULL);
#endif

    return 0;
}