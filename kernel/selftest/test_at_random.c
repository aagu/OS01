// kernel/selftest/test_at_random.c — auxv AT_RANDOM/AT_PLATFORM selftest
// （spec 2026-09-17 §7 Layer 2）。入口函数非 static：selftest.c 在
// #ifdef OS01_SELFTEST 块里前向声明并注册（deep_copy_argv 同模式）。
#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <random/random.h>   /* random_is_ready / kernel_random_get_strong /
                              * kernel_random_test_set_pool_quality */
#include <arch/random.h>     /* ARCH_ENTROPY_STRONG + arch_entropy_source_t */
#include <sched/task.h>       /* USER_STACK_BASE/TOP + task_selftest_auxv_probe */
#include <sys/auxv.h>
#include <string.h>
#include <stdint.h>

/* AAGU-5.7: AT_RANDOM 路径 STRONG-only 后，setup_user_stack 在 WEAK/NONE
 * 环境下返 -1。本文件测的是 auxv LAYOUT（与 entropy 质量无关）需要强制
 * STRONG 让 setup_user_stack 走通；同时 at_random_entropy 的 skip 条件
 * 改为"STRONG 可用"而非"pool ready"。两处都用 kernel_random_test_set_pool_quality
 * （random.c 在 OS01_SELFTEST 下导出）。 */

/* R11 MAJOR 修正: probe_argv / probe_envp 仍是奇数(2 argv + 1 envp)用;
 * even probe 必须用独立 3 元素数组,不能复用(否则偶数 case 越界读写)。 */
static char *odd_argv[]  = { (char *)"/bin/init", (char *)"arg1", NULL };
static char *odd_envp[]  = { (char *)"PATH=/bin", NULL };
static char *even_argv[] = { (char *)"/bin/init", (char *)"arg1", NULL };
static char *even_envp[] = { (char *)"ENV1",      (char *)"ENV2", NULL };

static int probe_build(char *const argv[], char *const envp[],
                       uint64_t *rsp, uint64_t *auxv, uint64_t *rnd,
                       uint64_t *plat)
{
    return task_selftest_auxv_probe(argv, envp, rsp, auxv, rnd, plat);
}

/* 通用 walk:三对齐全、值域/对齐正确、payload 非全零、rsp 16 对齐。
 * 抽出后 at_random_selftest_layout 与 _even 共享,避免代码重复。 */
static int at_random_selftest_walk(uint64_t rsp, uint64_t auxv,
                                   uint64_t rnd, uint64_t plat)
{
    const volatile uint64_t *av = (const volatile uint64_t *)auxv;

    int has_platform = 0, has_random = 0, has_null = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t t = av[2 * i], v = av[2 * i + 1];
        if (t == AT_NULL) { has_null = 1; break; }
        if (t == AT_PLATFORM) {
            has_platform = 1;
            if (v < USER_STACK_BASE || v >= USER_STACK_TOP) {
                serial_printk("[selftest] at_random: AT_PLATFORM a_val=%#lx out of stack\n",
                              (unsigned long)v);
                return -1;
            }
        }
        if (t == AT_RANDOM) {
            has_random = 1;
            if (v < USER_STACK_BASE || v >= USER_STACK_TOP || (v & 0xF) != 0) {
                serial_printk("[selftest] at_random: a_val=%#lx misranged/misaligned\n",
                              (unsigned long)v);
                return -1;
            }
        }
    }
    if (!has_platform || !has_random || !has_null) {
        serial_printk("[selftest] at_random: platform=%d random=%d null=%d\n",
                      has_platform, has_random, has_null);
        return -1;
    }
    if (strcmp((const char *)(uintptr_t)plat, "x86_64") != 0) {
        serial_printk("[selftest] at_random: AT_PLATFORM=\"%s\" want x86_64\n",
                      (const char *)(uintptr_t)plat);
        return -1;
    }
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)rnd;
    int nz = 0;
    for (int i = 0; i < 16; i++)
        if (p[i]) nz++;
    if (nz == 0) {
        serial_printk("[selftest] at_random: 16B payload all zero\n");
        return -1;
    }
    if (rsp & 0xF) {
        serial_printk("[selftest] at_random: rsp=%#lx misaligned\n",
                      (unsigned long)rsp);
        return -1;
    }
    return 0;
}

/* 布局用例(奇数分支,argv=2 envp=1 → argc+envc=3 → meta=48 → pad=8): 调 probe + walk */
int at_random_selftest_layout(void)
{
    /* AAGU-5.7: 强制 STRONG 让 setup_user_stack 走通；test 关注 layout
     * 而非 entropy 质量。Save/restore 避免污染后续 test (at_random_entropy
     * 的 STRONG-availability 探测 — 见下)。 */
    arch_entropy_source_t saved_q = random_get_pool_quality();
    bool saved_ready = random_is_ready();
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_STRONG, true);
    uint64_t rsp, auxv, rnd, plat;
    int rc = probe_build(odd_argv, odd_envp, &rsp, &auxv, &rnd, &plat);
    kernel_random_test_set_pool_quality(saved_q, saved_ready);
    if (rc != 0) {
        serial_printk("[selftest] at_random: probe build failed\n");
        return -1;
    }
    return at_random_selftest_walk(rsp, auxv, rnd, plat);
}

/* R10 MINOR + R11 MAJOR 修正: 偶数 argv+envp 分支验证 pad=0。
 * argv=2 + envp=2 → argc+envc=4 → meta=(4+3)*8=56 → fixed+meta=128
 *   → pad=(16-(128&15))&15=0 (偶数分支)。
 *
 * 必须用独立 3 元素数组 (even_argv/even_envp),**不**复用 odd_argv/odd_envp
 * (后者是 2 argv + 1 envp = 3 slots,偶数 case 当 3 envp 用会越界写 NULL)。
 * 直接调 probe + walk,不修改共享全局。 */
int at_random_selftest_layout_even(void)
{
    /* 同上 — STRONG pool → 测 layout → save/restore */
    arch_entropy_source_t saved_q = random_get_pool_quality();
    bool saved_ready = random_is_ready();
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_STRONG, true);
    uint64_t rsp, auxv, rnd, plat;
    int rc = probe_build(even_argv, even_envp, &rsp, &auxv, &rnd, &plat);
    kernel_random_test_set_pool_quality(saved_q, saved_ready);
    if (rc != 0) {
        serial_printk("[selftest] at_random_layout_even: probe build failed\n");
        return -1;
    }
    return at_random_selftest_walk(rsp, auxv, rnd, plat);
}

/* 熵用例：同一 argv 构建两次，16B payload 必须不同（CSPRNG）。
 * 缓冲是同一块静态数组——先拷贝第一次再重建。
 *
 * Issue AAGU-2 §1 + AAGU-5.7: AT_RANDOM 路径 STRONG-only。当 pool 未 STRONG-seeded
 * （NONE）或只 WEAK-seeded 且 arch 无 STRONG（默认 QEMU），kernel_random_get_strong
 * 返 false → setup_user_stack 返 -1。这是预期 fail-closed 行为；本测试只
 * 关心 STRONG-available 路径产生不同 keystream blocks。
 *
 * AAGU-5.7 reviewer 反馈：原 skip 条件只看 `random_is_ready()`（WEAK pool
 * 也算 ready）→ WEAK 模式下不 skip 但 probe 返 -1 → 测试错报"no AT_RANDOM pair"。
 * 改为直接探 `kernel_random_get_strong()` 是否能给出 STRONG（pool 或 arch 任一）。
 */
int at_random_selftest_entropy(void)
{
    /* 探一下 pool 真实能否提供 STRONG entropy（不消耗）：实际 probe_build
     * 之后会再调一次 setup_user_stack；这次探查只确认"STRONG-available"语义。 */
    uint8_t strong_probe[32];
    bool strong_avail = kernel_random_get_strong(strong_probe);
    memset(strong_probe, 0, 32);
    if (!strong_avail) {
        serial_printk("[selftest] at_random_entropy: STRONG not available "
                      "(pool WEAK or NONE + no arch STRONG, fail-closed) — skipped\n");
        return 0;   /* not a regression; secure behavior */
    }
    static uint8_t first[16];
    uint64_t rsp, auxv, rnd, plat;
    if (probe_build(odd_argv, odd_envp, &rsp, &auxv, &rnd, &plat) != 0 || rnd == 0) {
        serial_printk("[selftest] at_random_entropy: no AT_RANDOM pair\n");
        return -1;
    }
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)rnd;
    for (int i = 0; i < 16; i++) first[i] = p[i];
    if (probe_build(odd_argv, odd_envp, &rsp, &auxv, &rnd, &plat) != 0) return -1;
    int diff = 0;
    for (int i = 0; i < 16; i++)
        if (p[i] != first[i]) diff++;
    if (diff == 0) {
        serial_printk("[selftest] at_random_entropy: identical 16B payload\n");
        return -1;
    }
    return 0;
}
#endif /* OS01_SELFTEST */
