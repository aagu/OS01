/* @os01-test
 * id: test_aarch64_device_window
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: nightly
 * link: aarch64-device-window
 * scenarios: [memory.device_window.handoff]
 */
/* Runtime early device window (kernel/arch/aarch64/memory/
 * device_window.c) — the roadmap P2 hardcode item's kernel-side seam:
 * head.S maps Device blocks and the M1 arena/tree consumers follow the
 * window installed here from the boot_context early_mmio handoff.
 *
 * Cases: compile-time fallback without a handoff (zero-initialized TU,
 * no init call), flag-less handoff, valid handoff adoption, per-class
 * rejection (unaligned base, empty block count, over-cap block count),
 * the cap boundary itself, and the NULL reset path. Each case starts
 * from a reset so case order cannot mask a leak.
 */
#include "page_table_test_runner.h"
#include <core/bootinfo.h>
#include <arch/aarch64/device_window.h>
#include <arch/aarch64/boot_layout.h>

static struct aarch64_device_window dev;
static struct boot_context ctx;

static void expect_window(uint64_t base, uint64_t end)
{
    aarch64_device_window_get(&dev);
    assert_eq(base, dev.base_pa);
    assert_eq(end, dev.end_pa);
}

static void reset(void)
{
    memset(&ctx, 0, sizeof(ctx));
    boot_context_init(&ctx);
    aarch64_device_window_init(NULL);
}

static void test_device_window_fallback_without_init(void)
{
    aarch64_device_window_init(NULL);
    expect_window(AARCH64_BOOT_DEVICE_BASE, AARCH64_BOOT_DEVICE_END);
}

static void test_device_window_flagless_handoff_keeps_fallback(void)
{
    reset();
    aarch64_device_window_init(&ctx);
    expect_window(AARCH64_BOOT_DEVICE_BASE, AARCH64_BOOT_DEVICE_END);
}

static void test_device_window_valid_handoff_adopted(void)
{
    reset();
    ctx.early_mmio.console_base = 0x09000000;
    ctx.early_mmio.device_base = 0x08000000;
    ctx.early_mmio.device_blocks = 9;
    ctx.flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    aarch64_device_window_init(&ctx);
    expect_window(UINT64_C(0x08000000), UINT64_C(0x09200000));
}

static void test_device_window_null_reset(void)
{
    test_device_window_valid_handoff_adopted();
    aarch64_device_window_init(NULL);
    expect_window(AARCH64_BOOT_DEVICE_BASE, AARCH64_BOOT_DEVICE_END);
}

static void test_device_window_unaligned_base_rejected(void)
{
    reset();
    ctx.early_mmio.device_base = 0x08001000;
    ctx.early_mmio.device_blocks = 4;
    ctx.flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    aarch64_device_window_init(&ctx);
    expect_window(AARCH64_BOOT_DEVICE_BASE, AARCH64_BOOT_DEVICE_END);
}

static void test_device_window_zero_blocks_rejected(void)
{
    reset();
    ctx.early_mmio.device_base = 0x08000000;
    ctx.early_mmio.device_blocks = 0;
    ctx.flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    aarch64_device_window_init(&ctx);
    expect_window(AARCH64_BOOT_DEVICE_BASE, AARCH64_BOOT_DEVICE_END);
}

static void test_device_window_over_cap_rejected(void)
{
    reset();
    ctx.early_mmio.device_base = 0x08000000;
    ctx.early_mmio.device_blocks = 1025;
    ctx.flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    aarch64_device_window_init(&ctx);
    expect_window(AARCH64_BOOT_DEVICE_BASE, AARCH64_BOOT_DEVICE_END);
}

static void test_device_window_cap_boundary_adopted(void)
{
    reset();
    ctx.early_mmio.device_base = 0x08000000;
    ctx.early_mmio.device_blocks = 1024;
    ctx.flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    aarch64_device_window_init(&ctx);
    expect_window(UINT64_C(0x08000000), UINT64_C(0x88000000));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_device_window_fallback_without_init),
    TEST_ENTRY(test_device_window_flagless_handoff_keeps_fallback),
    TEST_ENTRY(test_device_window_valid_handoff_adopted),
    TEST_ENTRY(test_device_window_null_reset),
    TEST_ENTRY(test_device_window_unaligned_base_rejected),
    TEST_ENTRY(test_device_window_zero_blocks_rejected),
    TEST_ENTRY(test_device_window_over_cap_rejected),
    TEST_ENTRY(test_device_window_cap_boundary_adopted),
TEST_LIST_END

int main(void)
{
    return PAGE_TABLE_RUN_ALL_TESTS();
}
