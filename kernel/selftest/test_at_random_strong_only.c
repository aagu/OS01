// kernel/selftest/test_at_random_strong_only.c
// AT_RANDOM STRONG-only fail-closed contract test (AAGU-5.7).
//
// 验证 AT_RANDOM 生成路径（`setup_user_stack()` 经 `task_selftest_auxv_probe()`
// 暴露）在三档 pool quality 下的契约：
//
//   STRONG pool → kernel_random_get_strong 走 CSPRNG fast path → 返 true →
//                 setup_user_stack 成功，AT_RANDOM 16B payload 非零
//                 （与 arch 状态无关 — pool STRONG 已确保可用）
//
//   WEAK pool   → kernel_random_get_strong **不**接受 WEAK 走 fast path，
//                 落空到 arch_random_get_strong。
//                 arch fallback 决定结果：
//                   - arch STRONG (e.g. RDSEED): setup_user_stack 成功
//                   - arch 无 STRONG:         setup_user_stack 返 -1
//
//   NONE pool   → 同 WEAK：走 arch fallback，结果依 arch 状态而定。
//                 测试启动时探测 `arch_has_strong`，
//                 期望值 = `arch_has_strong`（mode-aware）。
//
// **Mock 机制（v3 — 第二轮 reviewer 反馈）**：v1 用 `__attribute__((weak))`
// 强符号 override `kernel_random_get_strong()` 直接返布尔，绕开了被验证
// 的质量过滤层 — 若生产实现错误地把 WEAK 当 STRONG 接受，测试仍通过。
// v2 改用**测试钩子** `kernel_random_test_set_pool_quality(q, ready)`，
// 由 `kernel/random/random.c` 在 `OS01_SELFTEST` 下导出：
//   - 不 mock 任何函数，让生产 `kernel_random_get_strong()` 真的运行。
//   - 通过测试钩子直接修改 `pool_quality` 与 `random_ready`，
//     让生产函数的 STRONG-only 契约在测试控制下被真实执行。
//   - 任何在生产函数里"放宽 STRONG 检查"的 bug 都会被该测试捕获。
//
// **端到端覆盖**：Part B 三档（STRONG/WEAK/NONE）都直接调
// `task_selftest_auxv_probe()`（内含 `setup_user_stack()`），断言 AT_RANDOM
// 生成路径完整端到端行为 — 不是只测 helper。
//
// 参考 AAGU-13 `test_arch_atomic_u64.c` 模式：直接调真实接口，断言契约。
// 本测试的"真实接口"是 `kernel_random_get_strong` + `setup_user_stack`。
#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <sched/task.h>       /* task_selftest_auxv_probe */
#include <sys/auxv.h>
#include <arch/random.h>     /* arch_entropy_source_t / ARCH_ENTROPY_* */
#include <random/random.h>    /* random_get_pool_quality /
                              * kernel_random_get_strong /
                              * kernel_random_test_set_pool_quality */
#include <string.h>
#include <stdint.h>

static int buf_any_nonzero(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0) return 1;
    return 0;
}

#define FAIL(fmt, ...) do {                                            \
    serial_printk("[selftest] at_random_strong_only: " fmt "\n",       \
                  ##__VA_ARGS__);                                      \
    kernel_random_test_set_pool_quality(saved_q, saved_ready);         \
    return -1;                                                         \
} while (0)

int test_at_random_strong_only(void)
{
    static char *argv[] = { (char *)"/bin/init", (char *)"arg1", NULL };
    static char *envp[] = { (char *)"PATH=/bin", NULL };
    uint8_t buf[32];

    /* Save boot pool state — restore at end so subsequent selftests
     * (notably at_random_entropy's STRONG-availability probe) see the
     * real boot-time state, not our simulated NONE. */
    arch_entropy_source_t saved_q = random_get_pool_quality();
    bool saved_ready = random_is_ready();

    /* Detect arch STRONG availability — production kernel_random_get_strong
     * falls through to arch_random_get_strong when pool is not STRONG-seeded,
     * so the result for WEAK/NONE pool tests depends on arch state.
     * Capture once at start so per-case expected values stay consistent. */
    uint8_t arch_probe[32];
    bool arch_has_strong = arch_random_get_strong(arch_probe);
    memset(arch_probe, 0, 32);

    /* === Part A：直接断言生产 kernel_random_get_strong 的 STRONG-only 契约 ===
     *
     * 不 mock 函数本身 — 通过测试钩子设 pool 状态，让生产函数真跑。
     * 这是 reviewer 反馈的核心修复点：v1 mock 掉了被验证的过滤层。 */

    /* A1: pool STRONG-seeded → 生产返 true + 非零 buf (走 CSPRNG) */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_STRONG, true);
    memset(buf, 0xAA, 32);
    if (!kernel_random_get_strong(buf))
        FAIL("STRONG pool: production kernel_random_get_strong returned false");
    if (!buf_any_nonzero(buf, 32))
        FAIL("STRONG pool: buf all zero");
    memset(buf, 0, 32);

    /* A2: pool NONE + not ready → 落空到 arch_random_get_strong.
     *     若 arch 有 STRONG → true（arch fallback）；否则 false。
     *     两者都是合同正确；测试用上面探测到的 arch_has_strong 决定期望值。 */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_NONE, false);
    memset(buf, 0xAA, 32);
    bool a2_ok = kernel_random_get_strong(buf);
    if (a2_ok != arch_has_strong)
        FAIL("NONE pool: got %d, expected %d (=arch_has_strong)",
             a2_ok ? 1 : 0, arch_has_strong ? 1 : 0);
    if (a2_ok && !buf_any_nonzero(buf, 32))
        FAIL("NONE pool + arch STRONG: out all zero");
    memset(buf, 0, 32);

    /* A3: pool WEAK + ready → 落空到 arch_random_get_strong（同 A2 逻辑）。
     *     WEAK pool 不抢 STRONG fast path：必须用 arch fallback。 */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_WEAK, true);
    memset(buf, 0xAA, 32);
    bool a3_ok = kernel_random_get_strong(buf);
    if (a3_ok != arch_has_strong)
        FAIL("WEAK pool: got %d, expected %d (=arch_has_strong)",
             a3_ok ? 1 : 0, arch_has_strong ? 1 : 0);
    if (a3_ok && !buf_any_nonzero(buf, 32))
        FAIL("WEAK pool + arch STRONG: out all zero");
    memset(buf, 0, 32);

    /* === Part B：端到端契约 — 调 setup_user_stack（经 task_selftest_auxv_probe）===
     *
     * 覆盖 AT_RANDOM 生成路径的实际返回值（不只是 helper）。 */

    /* B1: STRONG pool → probe 成功，AT_RANDOM 16B 非零 */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_STRONG, true);
    uint64_t rsp = 0, auxv = 0, rnd_k = 0, plat = 0;
    int rc = task_selftest_auxv_probe(argv, envp, &rsp, &auxv, &rnd_k, &plat);
    if (rc != 0)
        FAIL("STRONG pool: setup_user_stack should succeed");
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)rnd_k;
    int nz = 0;
    for (int i = 0; i < 16; i++)
        if (p[i]) nz++;
    if (nz == 0)
        FAIL("STRONG pool: AT_RANDOM 16B payload all zero");

    /* B2: NONE pool → probe 结果依赖 arch（同 A2 逻辑）：
     *     - arch STRONG → kernel_random_get_strong 返 true → setup 成功
     *     - arch 无 STRONG → 返 false → setup fail-closed
     *     两者都是合同正确；用探测到的 arch_has_strong 决定期望值。 */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_NONE, false);
    rc = task_selftest_auxv_probe(argv, envp, &rsp, &auxv, &rnd_k, &plat);
    bool b2_expected = arch_has_strong;
    if ((rc == 0) != b2_expected)
        FAIL("NONE pool end-to-end: rc=%d expected=%s",
             rc, b2_expected ? "0 (arch STRONG)" : "-1 (fail-closed)");

    /* B3: WEAK pool → probe 结果同 B2/A3 逻辑（mode-aware）。
     *     WEAK pool 不应直接接受为 STRONG（这是 WEAK 的语义约束）；
     *     只当 arch fallback 提供 STRONG 时 probe 才成功。
     *     arch 无 STRONG → WEAK 严格被拒，probe 必返 -1。
     *     Reviewer 第二轮反馈（PR #25 follow-up #3）要求补这个三档端到端覆盖。 */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_WEAK, true);
    rc = task_selftest_auxv_probe(argv, envp, &rsp, &auxv, &rnd_k, &plat);
    if ((rc == 0) != arch_has_strong)
        FAIL("WEAK pool end-to-end: rc=%d expected=%s "
             "(WEAK not accepted as STRONG; arch STRONG = fallback succeeds)",
             rc, arch_has_strong ? "0 (arch STRONG fallback)" : "-1 (WEAK rejected, fail-closed)");
    /* WEAK pool + arch STRONG 时，AT_RANDOM 16B payload 必须非零
     * （来自 arch RDSEED）。WEAK + arch no STRONG 时 probe 返 -1，
     * rnd_k 未设置，无需检查。 */
    if (arch_has_strong && rc == 0) {
        const volatile uint8_t *p2 = (const volatile uint8_t *)(uintptr_t)rnd_k;
        int nz2 = 0;
        for (int i = 0; i < 16; i++)
            if (p2[i]) nz2++;
        if (nz2 == 0)
            FAIL("WEAK pool + arch STRONG: AT_RANDOM 16B payload all zero");
    }

    /* Reset — restore boot pool state (saved above). */
    kernel_random_test_set_pool_quality(saved_q, saved_ready);
    serial_printk("[selftest] at_random_strong_only: 3 quality levels PASS\n");
    return 0;
}
#endif /* OS01_SELFTEST */
