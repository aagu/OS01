/* hosttests/cases/test_aarch64_ipi_broadcast.c — aarch64 ipi_broadcast
 * (logical vector → SGI mapping) 单元测试（M3 Task 7 Step 1 RED）。
 *
 * 编译【真实生产文件】kernel/arch/aarch64/intr/ipi.c +
 * kernel/arch/aarch64/intr/gic_driver.c（host clang，无修改）。
 * MMIO 是两个 mock 数组；gic_dev_current() / kpanic() 由本测试提供
 * （ipi.c 与 gic.c 解耦：gic.c 太重，host 不编）。
 *
 * 覆盖：
 *   1. gic_target_bit_inject() 注入 per-CPU GIC target byte（不依赖
 *      Task 8 的真实 GIC target 读取）；
 *   2. target_mask=0b10 + vector=IPI_VECTOR_TLB + inject(1,0x02)
 *      → GICD_SGIR = (sgi=3) | (0x02 << 16) | (FILTER_LIST=0 << 24)；
 *   3. target_mask=0 → 不写 SGIR；
 *   4. vector=0x41 (IPI_VECTOR_RESCHED, M3 暂不支持) → fatal hook（weak 覆盖）。
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <intr/ipi.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/gic.h>

/* 与 kernel/arch/aarch64/reg.h 同值（reg.h 带 target asm，host 不能 include） */
#define M_GICD_SGIR 0xF00

static uint32_t gicd_mock[0x400];
static uint32_t gicc_mock[0x20];
static struct gic_dev dev;

struct gic_dev *gic_dev_current(void) { return &dev; }

/* Task 8 起生产 ipi.c 引用 dtb 访问器（gic_target_bit_init）；
 * 本测试走 gic_target_bit_inject()，不走硬件探测 —— 给无害 mock 即可。 */
#include <arch/aarch64/dtb.h>
uint32_t dtb_cpu_count(void) { return 4; }
uint64_t dtb_gicd_base(void) { return 0; }

/* 覆盖 ipi.c 的 weak fatal hook：armed 时 longjmp 回测试点，
 * 否则 abort（不应到达）。 */
static jmp_buf panic_jb;
static int panic_armed;
void ipi_panic_unsupported_vector(uint32_t vector)
{
    (void)vector;
    if (panic_armed) {
        panic_armed = 0;
        longjmp(panic_jb, 1);
    }
    fprintf(stderr, "ipi fatal hook outside armed test\n");
    exit(2);
}

static uint32_t rd(const uint32_t *m, uint32_t off) { return m[off / 4]; }

static void mock_reset(void)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[0x004 / 4] = 0x1f;          /* TYPER: ITLines=31 → 1020 intids */
    gicd_mock[0x008 / 4] = 0x0200143b;    /* IIDR nonzero */
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
}

static void suite_sgi_mapping(void)
{
    TEST_SUITE("ipi_broadcast TLB -> SGI 3 (FILTER_LIST)");

    /* Case 2: mask=0b10, inject(1,0x02) → SGIR = 3 | 0x02<<16 | 0<<24 */
    mock_reset();
    gic_target_bit_inject(1, 0x02);
    ipi_broadcast(IPI_VECTOR_TLB, 0x2);
    assert_eq((uint32_t)((3u << 0) | (0x02u << 16) | (0u << 24)),
              rd(gicd_mock, M_GICD_SGIR));
    assert_eq((uint32_t)0x20003u, rd(gicd_mock, M_GICD_SGIR));

    /* mask 多位求或：CPU0=0x01 + CPU1=0x02 → targets byte 0x03 */
    mock_reset();
    gic_target_bit_inject(0, 0x01);
    gic_target_bit_inject(1, 0x02);
    ipi_broadcast(IPI_VECTOR_TLB, 0x3);
    assert_eq((uint32_t)(3u | (0x03u << 16)), rd(gicd_mock, M_GICD_SGIR));
}

static void suite_empty_mask(void)
{
    TEST_SUITE("ipi_broadcast empty mask writes no SGIR");
    mock_reset();
    ipi_broadcast(IPI_VECTOR_TLB, 0);
    assert_eq((uint32_t)0, rd(gicd_mock, M_GICD_SGIR));
}

static void suite_unsupported_vector(void)
{
    TEST_SUITE("ipi_broadcast unsupported vector panics");
    mock_reset();
    gic_target_bit_inject(1, 0x02);
    if (setjmp(panic_jb) == 0) {
        panic_armed = 1;
        ipi_broadcast(IPI_VECTOR_RESCHED, 0x2);   /* 期望 kpanic → longjmp */
        panic_armed = 0;
        assert_true(0);                           /* 未 panic = 失败 */
    } else {
        assert_true(1);
        assert_eq((uint32_t)0, rd(gicd_mock, M_GICD_SGIR));
    }
}

int main(void)
{
    suite_sgi_mapping(); suite_empty_mask(); suite_unsupported_vector();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_aarch64_ipi_broadcast",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
