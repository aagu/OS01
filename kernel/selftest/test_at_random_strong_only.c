// kernel/selftest/test_at_random_strong_only.c
// AT_RANDOM STRONG-only fail-closed contract test (AAGU-5.7).
//
// 验证 `setup_user_stack()` (经 `task_selftest_auxv_probe()` 暴露)
// 在三档 mock quality 下的契约：
//   STRONG → probe 返 0，AT_RANDOM 16B payload 非零
//   WEAK   → probe 返 -1（AT_RANDOM STRONG-only 拒绝）
//   NONE   → probe 返 -1（同上）
//
// **Mock 机制**：用 weak symbol override。
//   - `kernel/random/random.c` 把 `kernel_random_get_strong()` 标为
//     `__attribute__((weak))` —— 弱默认实现（生产代码）。
//   - 本文件提供**强**符号 `kernel_random_get_strong()` —— 在 KERNEL_SELFTEST=1
//     编译时，链接器以本文件为准。`g_mock_quality` 控制行为：
//       * `MOCK_DISABLED`（初值 -1）→ 走真实逻辑（pool STRONG →
//         get_random_bytes；否则 → arch_random_get_strong）。
//         与 weak 默认等价，保证后续 selftest 不被污染。
//       * `ARCH_ENTROPY_STRONG` (2) → 填非零模式，返回 true。
//       * `ARCH_ENTROPY_WEAK` (1)   → memset 0，返回 false（模拟 STRONG 拒绝）。
//       * `ARCH_ENTROPY_NONE` (0)   → memset 0，返回 false。
//
// **为什么单 boot 内模拟三档**：weak symbol 在 KERNEL_SELFTEST=1 编译时
// 覆盖默认实现，测试 set g_mock_quality 后调 `task_selftest_auxv_probe()`，
// 每次调都拿到期望契约。一份 selftest 三档全验证 — 与 QEMU mode-aware
// test 互补（mode-aware 验证真硬件；本测试验证契约本身）。
//
// 参考 AAGU-13 `test_arch_atomic_u64.c` 模式：直接调真实接口，断言契约。
// 本测试的"真实接口"是 task.c 调用方，mock 仅作用于 entropy 提供方。
#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <sched/task.h>       /* task_selftest_auxv_probe */
#include <sys/auxv.h>
#include <arch/random.h>     /* arch_entropy_source_t / ARCH_ENTROPY_* */
#include <random/random.h>    /* random_is_ready / random_get_pool_quality /
                               * get_random_bytes */
#include <string.h>
#include <stdint.h>

#define MOCK_DISABLED (-1)

/* Mock 状态：单 boot 内一个测试连续调 3 次，分别把 g_mock_quality 设
 * 为 STRONG/WEAK/NONE。Test 之间同步；test 退出前清回 MOCK_DISABLED
 * 防污染后续 selftest。 */
static int g_mock_quality = MOCK_DISABLED;

/* Public test helpers (AAGU-5.7) — let other selftest TUs force a quality
 * during their assertions, then reset. The strong override of
 * kernel_random_get_strong() consults g_mock_quality when set. */
void kernel_random_mock_set(arch_entropy_source_t q)
{
    g_mock_quality = (int)q;
}

void kernel_random_mock_reset(void)
{
    g_mock_quality = MOCK_DISABLED;
}

/* 强符号覆盖 — 链接时优先于 random.c 的 weak 默认。
 *
 * MOCK_DISABLED 分支重实现 random.c weak 默认的逻辑（pool STRONG →
 * get_random_bytes；否则 arch_random_get_strong）。约 8 行复制，
 * 与真实现同语义 — 保证 selftest 运行期间 kernel_random_get_strong
 * 行为不变（除被测试时）。 */
bool kernel_random_get_strong(uint8_t out[32])
{
    if (g_mock_quality == MOCK_DISABLED) {
        if (!out)
            return false;
        if (random_is_ready() && random_get_pool_quality() == ARCH_ENTROPY_STRONG) {
            get_random_bytes(out, 32);
            return true;
        }
        return arch_random_get_strong(out);
    }

    /* Mock active — simulate the requested quality's expected return. */
    switch ((arch_entropy_source_t)g_mock_quality) {
    case ARCH_ENTROPY_STRONG:
        /* 填 32B 非零模式（front 16B 会被 setup_user_stack 拷到 KSTACK
         * 作 AT_RANDOM payload；remaining 16B 立即 memset 0）。 */
        for (int i = 0; i < 32; i++) out[i] = (uint8_t)(0xA0 + i);
        return true;
    case ARCH_ENTROPY_WEAK:
    case ARCH_ENTROPY_NONE:
    default:
        /* WEAK/NONE: setup_user_stack 期望返 false (STRONG-only 拒绝)。 */
        memset(out, 0, 32);
        return false;
    }
}

#define FAIL(fmt, ...) do {                                            \
    serial_printk("[selftest] at_random_strong_only: " fmt "\n",       \
                  ##__VA_ARGS__);                                      \
    g_mock_quality = MOCK_DISABLED;                                    \
    return -1;                                                         \
} while (0)

int test_at_random_strong_only(void)
{
    /* 简单 argv/envp — 与现有 at_random_selftest_* 共用 odd 模式 */
    static char *argv[] = { (char *)"/bin/init", (char *)"arg1", NULL };
    static char *envp[] = { (char *)"PATH=/bin", NULL };

    /* 第 1 档：mocked STRONG — probe 应成功，AT_RANDOM 16B 非零。 */
    g_mock_quality = (int)ARCH_ENTROPY_STRONG;
    uint64_t rsp = 0, auxv = 0, rnd_k = 0, plat = 0;
    int rc = task_selftest_auxv_probe(argv, envp, &rsp, &auxv, &rnd_k, &plat);
    if (rc != 0)
        FAIL("STRONG simulation: probe returned %d (expected 0)", rc);
    if (rnd_k == 0)
        FAIL("STRONG simulation: AT_RANDOM pair missing from auxv");
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)rnd_k;
    int nz = 0;
    for (int i = 0; i < 16; i++)
        if (p[i]) nz++;
    if (nz == 0)
        FAIL("STRONG simulation: AT_RANDOM 16B payload all zero");

    /* 第 2 档：mocked WEAK — probe 应失败（AT_RANDOM STRONG-only 拒绝）。 */
    g_mock_quality = (int)ARCH_ENTROPY_WEAK;
    rc = task_selftest_auxv_probe(argv, envp, &rsp, &auxv, &rnd_k, &plat);
    if (rc == 0)
        FAIL("WEAK simulation: probe returned 0 "
             "(AT_RANDOM must reject WEAK, spec §6)");

    /* 第 3 档：mocked NONE — probe 应失败。 */
    g_mock_quality = (int)ARCH_ENTROPY_NONE;
    rc = task_selftest_auxv_probe(argv, envp, &rsp, &auxv, &rnd_k, &plat);
    if (rc == 0)
        FAIL("NONE simulation: probe returned 0 "
             "(AT_RANDOM must reject NONE, spec §6)");

    /* 清回 disabled — 防止污染后续 selftest 的真实 entropy 调用。 */
    g_mock_quality = MOCK_DISABLED;
    serial_printk("[selftest] at_random_strong_only: 3 quality levels PASS\n");
    return 0;
}
#endif /* OS01_SELFTEST */
