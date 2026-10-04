/* hosttests/cases/test_foundational_primitives.c — M3 Task 7 Step 4。
 *
 * 编译【真实生产文件】kernel/arch/aarch64/memory/vmm_gate.c（host clang，
 * 无修改；preinclude mock/vmm_gate_test_runtime.h 短路 percpu/spinlock）。
 * dtb_cpu_count() 和 percpu_data[] 由本测试提供。
 *
 * 覆盖：
 *   1. aarch64_pt_root_publish：幂等、root_pa==0 返回 false、注册表超限
 *      → vmm_gate_violation；aarch64_pt_root_is_published 查询；
 *   2. smp_starting_enter：单次发布，第二次 → violation；
 *   3. ipi_ready_publish_and_count：BSP/AP 共用同一函数，重复发布 →
 *      violation，计数正确；
 *   4. vmm_gate_check：smp_starting==0 直接放行；smp_starting==1 时
 *      ipi_ready_count < dtb_cpu_count() → violation，>= 则放行。
 *
 * 状态是进程级持久的，用例顺序即状态机（violation 用例全部通过
 * longjmp hook 捕获，不改变被测状态）。
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/vmm_gate.h>

/* percpu_data[] 由本测试定义（vmm_gate.o 只按符号名引用）。类型布局与
 * mock/vmm_gate_test_runtime.h 中 percpu_t 的尾部字段一致（16 字节）；
 * 刻意不复用 test_platform.h 的 percpu_t（它没有 ipi_ready/tlb_ack_gen）。 */
typedef struct {
    uint64_t self, need_resched;
    uint32_t ipi_ready, tlb_ack_gen;
} vmm_test_percpu_t;
vmm_test_percpu_t percpu_data[8];

static uint32_t mock_cpu_count_val = 8;
uint32_t dtb_cpu_count(void) { return mock_cpu_count_val; }

/* 覆盖 vmm_gate.c 的 weak violation hook */
static jmp_buf viol_jb;
static int viol_armed;
void vmm_gate_violation(const char *reason)
{
    (void)reason;
    if (viol_armed) {
        viol_armed = 0;
        longjmp(viol_jb, 1);
    }
    fprintf(stderr, "vmm_gate_violation outside armed test\n");
    exit(2);
}

/* 一个 violation = 一次断言通过。宏展开后 setjmp 与 longjmp 在同一
 * 函数帧内（statement expression），被测语句必须在 armed 区间内执行。 */
#define EXPECT_VIOLATION(stmt) __extension__ ({ \
    int _hit; \
    if (setjmp(viol_jb) == 0) { \
        viol_armed = 1; \
        stmt; \
        viol_armed = 0; \
        _hit = 0;                       /* 未触发 violation */ \
    } else { \
        _hit = 1;                       /* 已捕获 */ \
    } \
    _hit; })

static void suite_root_registry(void)
{
    TEST_SUITE("aarch64_pt_root_publish registry");

    /* 空 root：返回 false，不进注册表 */
    assert_false(aarch64_pt_root_publish(0));
    assert_false(aarch64_pt_root_is_published(&(uint64_t){0}));

    /* 首次发布 + 幂等重发布 */
    assert_true(aarch64_pt_root_publish(0x1000));
    assert_true(aarch64_pt_root_publish(0x1000));      /* 幂等 */
    assert_true(aarch64_pt_root_is_published(&(uint64_t){0x1000}));
    assert_false(aarch64_pt_root_is_published(&(uint64_t){0x2000}));

    /* 灌满 8 槽（0x1000 已占 1 槽，再灌 7 个） */
    for (uint64_t pa = 0x5000; pa <= 0xB000; pa += 0x1000)
        assert_true(aarch64_pt_root_publish(pa));

    /* 第 9 个不同 root → violation */
    assert_true(EXPECT_VIOLATION(aarch64_pt_root_publish(0x21000)));
}

static void suite_ipi_ready(void)
{
    TEST_SUITE("ipi_ready_publish_and_count");

    /* BSP(0) 与 AP(1) 用同一函数 */
    ipi_ready_publish_and_count(0);
    assert_true(percpu_data[0].ipi_ready == 1);
    ipi_ready_publish_and_count(1);
    assert_true(percpu_data[1].ipi_ready == 1);

    /* 重复发布 → violation（单次发布语义） */
    assert_true(EXPECT_VIOLATION(ipi_ready_publish_and_count(0)));
    assert_true(percpu_data[0].ipi_ready == 1);
}

static void suite_smp_starting_and_gate(void)
{
    TEST_SUITE("smp_starting_enter + vmm_gate_check");

    /* smp_starting==0：gate 直接放行（pre-SMP 例外） */
    vmm_gate_check();

    smp_starting_enter();
    /* 第二次 → violation，状态不回退 */
    assert_true(EXPECT_VIOLATION(smp_starting_enter()));

    /* smp_starting==1 后：count(2) < cpu_count(8) → violation */
    assert_true(EXPECT_VIOLATION(vmm_gate_check()));

    /* count(2) == cpu_count(2) → 放行 */
    mock_cpu_count_val = 2;
    vmm_gate_check();

    /* count(2) < cpu_count(3) → violation */
    mock_cpu_count_val = 3;
    assert_true(EXPECT_VIOLATION(vmm_gate_check()));
}

int main(void)
{
    suite_root_registry(); suite_ipi_ready(); suite_smp_starting_and_gate();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_foundational_primitives",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
