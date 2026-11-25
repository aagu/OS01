/* @os01-test
 * id: test_aarch64_uefi_boot_memory
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: aarch64-uefi-boot-memory
 * scenarios: [boot.uefi.aarch64_memory]
 */
#include "test_framework.h"
#include <core/bootinfo.h>
#include <arch/aarch64/uefi_memory_map.h>
#include "../../boot/uefi/arch/aarch64/loader.h"
#include <string.h>

struct test_efi_desc {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
    uint64_t extra;
};

static void test_fw_dtb_readable_rules(void)
{
    struct test_efi_desc single = {
        .type = AARCH64_UEFI_MEM_RESERVED,
        .phys_start = UINT64_C(0x1f0000),
        .num_pages = 32u,
        .attribute = AARCH64_UEFI_ATTR_WB,
    };
    struct test_efi_desc split[2] = {
        {
            .type = AARCH64_UEFI_MEM_RESERVED,
            .phys_start = UINT64_C(0x1f0000),
            .num_pages = 16u,
            .attribute = AARCH64_UEFI_ATTR_WB,
        },
        {
            .type = AARCH64_UEFI_MEM_CONVENTIONAL,
            .phys_start = UINT64_C(0x200000),
            .num_pages = 16u,
            .attribute = AARCH64_UEFI_ATTR_WB,
        },
    };
    struct test_efi_desc hole[2] = {
        {
            .type = AARCH64_UEFI_MEM_RESERVED,
            .phys_start = UINT64_C(0x1f0000),
            .num_pages = 15u,
            .attribute = AARCH64_UEFI_ATTR_WB,
        },
        {
            .type = AARCH64_UEFI_MEM_RESERVED,
            .phys_start = UINT64_C(0x200000),
            .num_pages = 16u,
            .attribute = AARCH64_UEFI_ATTR_WB,
        },
    };
    struct test_efi_desc mmio = single;
    struct test_efi_desc rp = single;

    mmio.type = AARCH64_UEFI_MEM_MMIO;
    rp.attribute = AARCH64_UEFI_ATTR_WB | AARCH64_UEFI_ATTR_RP;

    assert_true(aarch64_fw_dtb_readable(&single, sizeof(single), 1u,
                                        UINT64_C(0x1f0000), 40u));
    assert_true(aarch64_fw_dtb_readable(&single, sizeof(single), 1u,
                                        UINT64_C(0x1f0000), 0x20000u));
    assert_true(aarch64_fw_dtb_readable(split, sizeof(split[0]), 2u,
                                        UINT64_C(0x1f0000), 0x20000u));

    assert_false(aarch64_fw_dtb_readable(&mmio, sizeof(mmio), 1u,
                                         UINT64_C(0x1f0000), 40u));
    assert_false(aarch64_fw_dtb_readable(&rp, sizeof(rp), 1u,
                                         UINT64_C(0x1f0000), 40u));
    assert_false(aarch64_fw_dtb_readable(hole, sizeof(hole[0]), 2u,
                                         UINT64_C(0x1f0000), 0x20000u));
    assert_false(aarch64_fw_dtb_readable(&single, sizeof(single), 1u,
                                         UINT64_C(0x1f0000), 0x20001u));
    assert_false(aarch64_fw_dtb_readable(&single, sizeof(single), 1u,
                                         UINT64_C(0x1f0000), UINT64_MAX));
}

static void test_early_mmio_map_safe_and_overlap_rules(void)
{
    struct test_efi_desc map[2];
    struct boot_context ctx;

    memset(map, 0, sizeof(map));
    map[0].type = AARCH64_UEFI_MEM_CONVENTIONAL;
    map[0].phys_start = UINT64_C(0x40000000);
    map[0].num_pages = 0x1000u; /* [0x40000000, 0x41000000) */
    map[0].attribute = AARCH64_UEFI_ATTR_WB;

    map[1].type = AARCH64_UEFI_MEM_MMIO;
    map[1].phys_start = UINT64_C(0x08000000);
    map[1].num_pages = 0x1200u; /* [0x08000000, 0x09200000) */

    boot_context_init(&ctx);
    ctx.flags = BOOT_CONTEXT_HAS_MEMORY_MAP | BOOT_CONTEXT_HAS_EARLY_MMIO;
    ctx.memory.entries = (uint64_t)(uintptr_t)map;
    ctx.memory.entry_size = sizeof(map[0]);
    ctx.memory.entry_count = 2u;
    ctx.memory.format = BOOT_MEMORY_FORMAT_UEFI_RAW;
    ctx.early_mmio.console_base = UINT64_C(0x09000000);
    ctx.early_mmio.device_base = UINT64_C(0x08000000);
    ctx.early_mmio.device_blocks = 9u;

    assert_true(aarch64_early_mmio_map_safe(&ctx));

    /* Adjacent RAM boundary: [0x3fe00000, 0x40000000) does not overlap [0x40000000, ...) */
    assert_eq(0, aarch64_uefi_range_overlaps_ram(map, sizeof(map[0]), 2u,
                                                 UINT64_C(0x3fe00000),
                                                 UINT64_C(0x40000000)));
    assert_eq(1, aarch64_uefi_range_overlaps_ram(map, sizeof(map[0]), 2u,
                                                 UINT64_C(0x3fe00000),
                                                 UINT64_C(0x40001000)));

    /* Window covering conventional RAM -> rejected */
    ctx.early_mmio.device_base = UINT64_C(0x40000000);
    ctx.early_mmio.device_blocks = 1u;
    ctx.early_mmio.console_base = UINT64_C(0x40000000);
    assert_false(aarch64_early_mmio_map_safe(&ctx));

    /* Re-evaluation under second map key where RAM classification changes */
    ctx.early_mmio.device_base = UINT64_C(0x08000000);
    ctx.early_mmio.device_blocks = 9u;
    ctx.early_mmio.console_base = UINT64_C(0x09000000);
    assert_true(aarch64_early_mmio_map_safe(&ctx));

    map[1].type = AARCH64_UEFI_MEM_CONVENTIONAL;
    assert_false(aarch64_early_mmio_map_safe(&ctx));

    /* Off-window firmware console whose +0x48 crosses end of its 2 MiB block */
    map[1].type = AARCH64_UEFI_MEM_MMIO;
    ctx.flags |= BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED;
    ctx.early_mmio.console_base = UINT64_C(0x1f001fffc0);
    assert_false(aarch64_early_mmio_map_safe(&ctx));
}

int main(void)
{
    TEST_SUITE("aarch64_uefi_boot_memory");
    test_fw_dtb_readable_rules();
    test_early_mmio_map_safe_and_overlap_rules();
    TEST_RESULTS();
    return __test_stats.failed ? 1 : 0;
}
