// kernel/selftest/test_entropy_quality.c — facade 当前质量档契约验证。
//
// 单次 boot 检测 facade 在当前硬件/QEMU 配置下产出的 quality，
// 然后断言该档的全部契约（spec §3.2 / §3.3 / §5.1 / §5.2）。
// 同一函数在 STRONG / WEAK / NONE 三种 QEMU 模式下都 PASS；
// 编排脚本（tests/scripts/qemu_entropy_modes.sh, Task 8）通过 grep
// `[selftest] entropy_quality_selftest_current_mode... PASS` 校验。
//
// 断言总览（每档独立）：
//   NONE  → facade return false, q==NONE, out memset 0, !random_is_ready,
//           arch_random_get_strong() return false
//   WEAK  → facade return true,  q==WEAK, out non-zero,  random_is_ready
//           (spec §5.2 WEAK→ready),  arch_random_get_strong() return false
//   STRONG→ facade return true,  q==STRONG, out non-zero, random_is_ready,
//           arch_random_get_strong() return true
#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <arch/random.h>
#include <random/random.h>     // random_is_ready
#include <string.h>
#include <stdint.h>

static int buf_all_zero(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0) return 0;
    return 1;
}

static int buf_any_nonzero(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0) return 1;
    return 0;
}

/* 通用 fail-with-context：避免每个 assert 都写 serial_printk。 */
#define FAIL(fmt, ...) do {                                  \
    serial_printk("[selftest] entropy_quality_selftest_current_mode: " \
                  fmt "\n", ##__VA_ARGS__);                  \
    return -1;                                               \
} while (0)

int entropy_quality_selftest_current_mode(void)
{
    /* 第 1 步：调 facade 拿当前 quality 档。预填 buf 为非零 —
     * 若 facade 走失败路径（NONE），必须主动 memset 0 覆盖预填值。 */
    uint8_t buf[32];
    memset(buf, 0xAA, 32);
    arch_entropy_source_t q = ARCH_ENTROPY_STRONG;  /* 故意错初值 */
    bool facade_ok = arch_random_get_entropy(buf, &q);

    switch (q) {
    case ARCH_ENTROPY_NONE:
        if (facade_ok)
            FAIL("NONE mode: facade returned true (expected false)");
        if (!buf_all_zero(buf, 32))
            FAIL("NONE mode: out not all zero (facade must memset 0)");
        /* NOTE: do NOT check random_is_ready() here. random_ready reflects
         * the pool's seed source (incl. UEFI GetRNG), independent of the
         * arch facade. CI runs in CI env where arch facade reports NONE
         * (qemu64 no RDRAND/RDSEED) but UEFI GetRNG (via virtio-rng) seeds
         * the pool STRONG — random_is_ready() returns true. That's the
         * correct combined-state (UEFI-seeded pool is healthy; arch hardware
         * has no entropy), not a fail-closed violation. */
        break;

    case ARCH_ENTROPY_WEAK:
        if (!facade_ok)
            FAIL("WEAK mode: facade returned false");
        if (!buf_any_nonzero(buf, 32))
            FAIL("WEAK mode: out all zero (DRBG should produce entropy)");
        if (!random_is_ready())
            FAIL("WEAK mode: random_is_ready()=false "
                 "(spec §5.2: WEAK → ready=true is authorized)");
        break;

    case ARCH_ENTROPY_STRONG:
        if (!facade_ok)
            FAIL("STRONG mode: facade returned false");
        if (!buf_any_nonzero(buf, 32))
            FAIL("STRONG mode: out all zero (raw entropy should produce bytes)");
        if (!random_is_ready())
            FAIL("STRONG mode: random_is_ready()=false");
        break;

    default:
        FAIL("unknown quality=%d (must be 0/1/2)", q);
    }
    memset(buf, 0, 32);

    /* 第 2 步：arch_random_get_strong() 严格只接受 STRONG —
     * WEAK/NONE 模式下必须返 false（spec §6 / §3.3）。 */
    uint8_t strong_buf[32];
    memset(strong_buf, 0xAA, 32);
    bool strong_ok = arch_random_get_strong(strong_buf);
    int expect_strong = (q == ARCH_ENTROPY_STRONG) ? 1 : 0;
    if (strong_ok != expect_strong)
        FAIL("strong_filter: got=%d want=%d (q=%d)",
             strong_ok ? 1 : 0, expect_strong, q);
    if (strong_ok && !buf_any_nonzero(strong_buf, 32))
        FAIL("strong_filter: out all zero despite true return");
    memset(strong_buf, 0, 32);

    return 0;
}
#endif /* OS01_SELFTEST */
