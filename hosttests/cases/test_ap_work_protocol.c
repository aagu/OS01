/* hosttests/cases/test_ap_work_protocol.c — ap_work 协议单元测试（M3 Task 10
 * Step 1 RED）。
 *
 * 编译【真实生产文件】kernel/arch/aarch64/smp/ap_work.c（host clang，无修改；
 * -include mock/ap_work_test_runtime.h 短路 <arch/cpu.h> 与
 * <arch/aarch64/boot_log.h>）。BSP mock 与 AP mock 用同一 ap_work[] 数组
 * （BSS 静态，见 kernel/include/arch/aarch64/ap_work.h）。
 *
 * 覆盖（brief Step 1 四点）：
 *   1. BSP submit（seq=1, cmd=READ64, arg=VA）→ AP ap_work_run_one 收到并
 *      模拟读 → DONE → BSP ap_work_wait 读到 out，槽回到 IDLE；
 *   2. 第二次 submit seq=2 → AP 消费新请求；AP 不会把旧 DONE 当新请求
 *      （对同一 seq 重复 run_one 是 no-op；seq 不匹配的 wait 返回 false）；
 *   3. 超时：AP 不写 DONE → ap_work_wait 打印
 *      "WORK-TIMEOUT cpu=%u seq=%u" +
 *      "M3-SHOOTDOWN-PROBE: FAIL work-timeout" 并 for(;;) arch_cpu_halt()
 *      （host 用 longjmp hook 证明到达 halt 路径）；
 *   4. seq 首项 = 1：AP "最后已消费 seq" 初值 = 0，因此 seq=0 的 READY
 *      请求被忽略（防止 last_consumed 初值 0 吃掉合法请求）。
 *
 * 另外：WORK_BARRIER 完成且不改写 out。
 */
#include <test_framework.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
/* NOTE: no <stdatomic.h> on this host config (wchar_t), same as the
 * freestanding aarch64 kernel — use __atomic builtins. */
#include <string.h>
#include <arch/aarch64/ap_work.h>

/* ── mock runtime（ap_work_test_runtime.h 声明，这里给实现） ── */

static char log_buf[512];
static size_t log_len;

void kputs(const char *s)
{
    while (*s && log_len + 1 < sizeof(log_buf))
        log_buf[log_len++] = *s++;
    log_buf[log_len] = '\0';
}

void kputu(uint64_t v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) {
        kputs("0");
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        char one[2] = { tmp[--i], '\0' };
        kputs(one);
    }
}

static uint64_t fake_cycles;
static uint64_t cycle_step = 1;

uint64_t arch_cycle_counter(void)
{
    fake_cycles += cycle_step;
    return fake_cycles;
}

static jmp_buf halt_jb;
static int halt_armed;

void arch_cpu_halt(void)
{
    if (halt_armed) {
        halt_armed = 0;
        longjmp(halt_jb, 1);
    }
    fprintf(stderr, "arch_cpu_halt outside armed test\n");
    exit(2);
}

void arch_cpu_pause(void)
{
    /* host 上 yield 即空操作 */
}

static void mock_reset(void)
{
    memset(log_buf, 0, sizeof(log_buf));
    log_len = 0;
    fake_cycles = 0;
    cycle_step = 1;
    halt_armed = 0;
    /* 生产侧 ap_work[] 是 BSS（零 = 全 IDLE）；host 上手动清零模拟。 */
    for (uint32_t cpu = 0; cpu < NR_CPUS; cpu++)
        __atomic_store_n(&ap_work[cpu].state, AP_WORK_IDLE,
                              __ATOMIC_RELAXED);
}

/* ── tests ── */

/* __atomic_load_n on an _Atomic field yields an _Atomic-qualified value
 * that assert_eq cannot cast — funnel through a plain uint32_t. */
static uint32_t state_of(uint32_t cpu)
{
    return __atomic_load_n(&ap_work[cpu].state, __ATOMIC_ACQUIRE);
}

static void test_read64_roundtrip(void)
{
    TEST_SUITE("read64 roundtrip");
    mock_reset();

    uint64_t target = 0xDEADBEEFCAFEBABEULL;
    uint64_t out = 0;

    ap_work_submit(1, 1, WORK_READ64, (uint64_t)(uintptr_t)&target, 0);
    assert_eq(AP_WORK_READY, state_of(1));
    assert_eq(1, ap_work[1].seq);
    assert_eq(WORK_READ64, ap_work[1].cmd);

    /* AP mock：进工作循环消费一项 */
    ap_work_run_one(1);
    assert_eq(AP_WORK_DONE, state_of(1));

    /* BSP mock：等待完成并读回 */
    assert_eq(true, ap_work_wait(1, 1, &out, 1000000));
    assert_eq(0xDEADBEEFCAFEBABEULL, out);
    /* BSP 置回 IDLE（槽可复用） */
    assert_eq(AP_WORK_IDLE, state_of(1));
}

static void test_seq_guard_and_second_submit(void)
{
    TEST_SUITE("seq guard / second submit");
    mock_reset();

    uint64_t var_a = 0x1111ULL, var_b = 0x2222ULL, out = 0;

    ap_work_submit(2, 1, WORK_READ64, (uint64_t)(uintptr_t)&var_a, 0);
    ap_work_run_one(2);
    assert_eq(AP_WORK_DONE, state_of(2));

    /* AP 对同一槽重复 run_one：state 已 DONE，不是 READY → no-op */
    ap_work_run_one(2);
    assert_eq(AP_WORK_DONE, state_of(2));

    /* 第二次 submit seq=2：AP 不把旧 DONE 当新请求 —— seq 匹配才消费 */
    ap_work_submit(2, 2, WORK_READ64, (uint64_t)(uintptr_t)&var_b, 0);
    assert_eq(AP_WORK_READY, state_of(2));
    ap_work_run_one(2);
    assert_eq(AP_WORK_DONE, state_of(2));

    /* 旧 seq 的 wait 必须失败（防读旧 DONE），且不置 IDLE */
    assert_eq(false, ap_work_wait(2, 1, &out, 1000000));
    assert_eq(AP_WORK_DONE, state_of(2));

    /* 新 seq 的 wait 成功，读到 var_b */
    assert_eq(true, ap_work_wait(2, 2, &out, 1000000));
    assert_eq(0x2222ULL, out);
    assert_eq(AP_WORK_IDLE, state_of(2));

    /* out=NULL 的 wait 也合法（BARRIER 场景） */
    ap_work_submit(2, 3, WORK_BARRIER, 0, 0);
    ap_work_run_one(2);
    assert_eq(true, ap_work_wait(2, 3, NULL, 1000000));
    assert_eq(AP_WORK_IDLE, state_of(2));
}

static void test_timeout_fatal(void)
{
    TEST_SUITE("timeout FATAL");
    mock_reset();

    /* AP 永不消费：submit 后直接等一个极小的 deadline */
    ap_work_submit(1, 5, WORK_READ64, 0x1000, 0);
    cycle_step = 100; /* 每次读 counter 大步推进 */

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        (void)ap_work_wait(1, 5, NULL, 10);
        /* 不应到达：超时必须走 halt 终态 */
        assert_true(0);
        return;
    }

    kputs("");
    assert_true(strstr(log_buf, "WORK-TIMEOUT cpu=1 seq=5") != NULL);
    assert_true(strstr(log_buf,
                       "M3-SHOOTDOWN-PROBE: FAIL work-timeout") != NULL);
    /* 超时终态不复用槽：state 保持 READY，不被置回 IDLE */
    assert_eq(AP_WORK_READY, state_of(1));
}

static void test_seq_first_is_one(void)
{
    TEST_SUITE("seq first = 1 / last_consumed init 0");
    mock_reset();

    /* AP "最后已消费 seq" 初值 = 0（BSS 静态）：若 AP 看到一个
     * READY 但 seq=0 的槽，必须忽略而不是消费（否则 last_consumed
     * 的初值 0 会与首项 seq=1 的约定冲突）。生产协议里 seq 首项
     * 恒为 1，seq=0 不会被 submit；此处直接操纵槽验证防线存在。 */
    ap_work[3].cmd = WORK_BARRIER;
    ap_work[3].seq = 0;
    __atomic_store_n(&ap_work[3].state, AP_WORK_READY,
                          __ATOMIC_RELEASE);
    ap_work_run_one(3);
    assert_eq(AP_WORK_READY, state_of(3));

    /* 首项 seq=1 正常消费（证明忽略只针对 seq=0 的防线，不是坏状态） */
    ap_work_submit(3, 1, WORK_BARRIER, 0, 0);
    ap_work_run_one(3);
    assert_eq(AP_WORK_DONE, state_of(3));
    assert_eq(true, ap_work_wait(3, 1, NULL, 1000000));

    /* IDLE 槽上 run_one 是 no-op */
    ap_work_run_one(3);
    assert_eq(AP_WORK_IDLE, state_of(3));
}

int main(void)
{
    test_read64_roundtrip();
    test_seq_guard_and_second_submit();
    test_timeout_fatal();
    test_seq_first_is_one();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_ap_work_protocol",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
