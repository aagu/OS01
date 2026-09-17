/* hosttests/cases/test_gic_driver.c — GICv2 driver 单元测试（spec §4.1/G1）。
 *
 * 编译【真实生产文件】kernel/arch/aarch64/gic_driver.c（host clang，无修改），
 * MMIO 是两个 mock 数组。覆盖: init(TYPER/IIDR)、分类、enable/disable、
 * priority/targets、handler 注册表、unexpected 回调、SGIR 编码、
 * set/clear pending、IAR/EOIR 往返(含 CPUID 位)、dispatch 三分支
 * (spurious/unexpected/命中) + dispatch-CPUID case (R1-9)。
 *
 * R1-4: dispatch 用例一律传 NULL regs——gic.h 只前向声明 struct pt_regs，
 * 本测试绝不定义 pt_regs 对象（不完整类型不可定义对象）。
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>

/* 与 kernel/arch/aarch64/reg.h 同值（reg.h 带 target asm，host 不能 include） */
#define M_GICD_CTLR       0x000
#define M_GICD_TYPER      0x004
#define M_GICD_IIDR       0x008
#define M_GICD_IGROUPR    0x080
#define M_GICD_ISENABLER  0x100
#define M_GICD_ICENABLER  0x180
#define M_GICD_ISPENDR    0x200
#define M_GICD_ICPENDR    0x280
#define M_GICD_IPRIORITYR 0x400
#define M_GICD_ITARGETSR  0x800
#define M_GICD_SGIR       0xF00
#define M_GICC_CTLR       0x000
#define M_GICC_PMR        0x004
#define M_GICC_IAR        0x00C
#define M_GICC_EOIR       0x010

static uint32_t gicd_mock[0x400];
static uint32_t gicc_mock[0x20];
static struct gic_dev dev;

static void mock_reset(uint32_t typer_itlines)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[M_GICD_TYPER / 4] = typer_itlines & 0x1f;
    gicd_mock[M_GICD_IIDR / 4] = 0x0200143b;   /* QEMU virt GICv2 IIDR 非零 */
}
static uint32_t rd(const uint32_t *m, uint32_t off) { return m[off / 4]; }

/* handler 注册表探针（regs 恒为 NULL，handler 不解引用） */
static uint32_t hit_intid; static uint64_t hit_param; static uint32_t hit_calls;
static void probe_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{ (void)regs; hit_intid = intid; hit_param = param; ++hit_calls; }

/* unexpected 回调探针（R1-3 的 hosttest 侧覆盖） */
static uint32_t unexpected_hits; static uint32_t unexpected_last;
static void probe_unexpected(uint32_t intid)
{ ++unexpected_hits; unexpected_last = intid; }

/* per-CPU IAR trace 的 cpu-index 探针（R2-6 的 hosttest 侧覆盖） */
static uint32_t mock_cpu_index; static uint32_t mock_cpu_index_calls;
static uint32_t probe_cpu_index(void) { ++mock_cpu_index_calls; return mock_cpu_index; }

static void suite_init(void)
{
    TEST_SUITE("gic_dev_init");
    mock_reset(2);                                   /* ITLinesNumber=2 → 96 intids */
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    assert_eq(96, (int)dev.nr_intids);
    mock_reset(2); gicd_mock[M_GICD_IIDR / 4] = 0;   /* IIDR=0 → 拒绝 */
    assert_true(gic_dev_init(&dev, gicd_mock, gicc_mock) != 0);
    assert_true(gic_dev_init(&dev, 0, gicc_mock) != 0);      /* NULL gicd */
    assert_true(gic_dev_init(&dev, gicd_mock, 0) != 0);      /* NULL gicc */
}

static void suite_classify(void)
{
    TEST_SUITE("gic_irq_type");
    assert_eq(GIC_IRQ_SGI, gic_irq_type(0));
    assert_eq(GIC_IRQ_SGI, gic_irq_type(15));
    assert_eq(GIC_IRQ_PPI, gic_irq_type(16));
    assert_eq(GIC_IRQ_PPI, gic_irq_type(30));        /* CNTP */
    assert_eq(GIC_IRQ_SPI, gic_irq_type(32));
    assert_eq(GIC_IRQ_SPI, gic_irq_type(33));        /* PL011 RX */
    assert_eq(GIC_IRQ_SPI, gic_irq_type(1019));
    assert_eq(GIC_IRQ_INVALID, gic_irq_type(1020));
    assert_eq(GIC_IRQ_INVALID, gic_irq_type(1023));
    assert_eq(GIC_IRQ_INVALID, gic_irq_type(4000));
}

static void suite_enable(void)
{
    TEST_SUITE("enable/disable/priority/targets");
    mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
    /* SPI 33: ISENABLER[1] bit1, 优先级字节 33, 目标 byte → cpu0。
     * R2-2: byte 寄存器地址 = BASE + (33/4)*4 = BASE + 32; 33%4=1 → >>8。 */
    assert_eq(0, gic_irq_config(&dev, 33, true, 0x00, 0x01));
    assert_eq((uint32_t)(1u << 1), rd(gicd_mock, M_GICD_ISENABLER + 4));
    assert_eq(0x00u, (rd(gicd_mock, M_GICD_IPRIORITYR + 32) >> 8) & 0xff);
    assert_eq(0x01u, (rd(gicd_mock, M_GICD_ITARGETSR + 32) >> 8) & 0xff);
    assert_eq(0u, rd(gicd_mock, M_GICD_IGROUPR + 4) & (1u << 1));   /* 组0 */
    /* disable 走 ICENABLER */
    assert_eq(0, gic_irq_disable(&dev, 33));
    assert_eq((uint32_t)(1u << 1), rd(gicd_mock, M_GICD_ICENABLER + 4));
    /* 越界拒绝 */
    assert_true(gic_irq_enable(&dev, 96) != 0);      /* == nr_intids */
    assert_true(gic_irq_enable(&dev, 2000) != 0);
}

static void suite_registry(void)
{
    TEST_SUITE("handler registry");
    gic_unregister_handler(35);                       /* 未注册 → 容错 */
    assert_eq(0, gic_register_handler(35, probe_handler, 0x1234, "t35"));
    assert_true(gic_register_handler(35, probe_handler, 0, "dup") != 0);
    assert_true(gic_register_handler(96, probe_handler, 0, "oor") != 0);
    uint64_t param = 0;
    assert_true((uintptr_t)gic_get_handler(35, &param) == (uintptr_t)probe_handler);
    assert_eq(0x1234, (int)param);
    assert_true(gic_get_handler(36, &param) == 0);
    assert_eq(0, gic_unregister_handler(35));
    assert_true(gic_get_handler(35, &param) == 0);
}

static void suite_ack_eoi(void)
{
    TEST_SUITE("IAR/EOIR roundtrip + per-CPU trace");
    mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
    uint32_t iar = (2u << 10) | 35u;                  /* SGI 风格: CPUID=2 */
    gicc_mock[M_GICC_IAR / 4] = iar;
    assert_eq((int)iar, (int)gic_ack(&dev));          /* 原始值, 含 CPUID */
    assert_eq((int)iar, (int)dev.dbg_last_iar);       /* 单槽 trace（无 hook 时） */
    gic_eoi(&dev, iar);
    assert_eq((int)iar, (int)rd(gicc_mock, M_GICC_EOIR));  /* 原样写回 — D7 修复 */
    /* per-CPU trace (R2-6): 注入 cpu-index hook 后, ack 写本 CPU 槽,
     * 单槽字段照写（向后兼容）, 其他核槽不受影响。 */
    gic_driver_set_cpu_index(probe_cpu_index);
    mock_cpu_index = 3; gicc_mock[M_GICC_IAR / 4] = (3u << 10) | 7u;
    assert_eq(0xC07, (int)gic_ack(&dev));
    assert_eq(0xC07, (int)gic_driver_trace_get(3));   /* 本 CPU 槽 */
    assert_eq(0, (int)gic_driver_trace_get(0));       /* 其他槽仍为 0 */
    mock_cpu_index = 0; gicc_mock[M_GICC_IAR / 4] = 99u;
    assert_eq(99, (int)gic_ack(&dev));
    assert_eq(99, (int)gic_driver_trace_get(0));
    assert_eq(0xC07, (int)gic_driver_trace_get(3));   /* 旧槽不被跨"核"覆写 */
    assert_true(mock_cpu_index_calls >= 2);
    gic_driver_set_cpu_index(NULL);                   /* 清除 → 回退单槽路径 */
    gicc_mock[M_GICC_IAR / 4] = 55u;
    assert_eq(55, (int)gic_ack(&dev));
    assert_eq(55, (int)dev.dbg_last_iar);
    gicc_mock[M_GICC_IAR / 4] = 1023;                 /* spurious */
    assert_eq(1023, (int)gic_ack(&dev));
}

static void suite_dispatch(void)
{
    TEST_SUITE("gic_dev_dispatch (NULL regs)");
    mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
    hit_calls = 0; unexpected_hits = 0;
    assert_eq(0, gic_register_handler(35, probe_handler, 0x5678, "d35"));
    gicc_mock[M_GICC_IAR / 4] = 35;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, (int)hit_calls);
    assert_eq(35, (int)hit_intid);
    assert_eq(0x5678, (int)hit_param);
    assert_eq(35, (int)rd(gicc_mock, M_GICC_EOIR));   /* handler 后 EOI */
    /* spurious: 不写 EOIR、不调 handler、不触发 unexpected */
    gicc_mock[M_GICC_IAR / 4] = 1023; gicc_mock[M_GICC_EOIR / 4] = 0;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, (int)hit_calls);
    assert_eq(0, (int)rd(gicc_mock, M_GICC_EOIR));
    assert_eq(0, (int)unexpected_hits);
    /* unexpected: 无 handler → unexpected_cb(intid) + 仍 EOI（防 GIC 锁死,
     * 对齐 time.c:105-115 语义）。R1-3: 回调可注入/可清除 */
    gic_driver_set_unexpected(probe_unexpected);
    gicc_mock[M_GICC_IAR / 4] = 60;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, (int)hit_calls);
    assert_eq(60, (int)rd(gicc_mock, M_GICC_EOIR));
    assert_eq(1, (int)unexpected_hits);
    assert_eq(60, (int)unexpected_last);
    gic_driver_set_unexpected(NULL);                  /* 清除后不再触发 */
    gicc_mock[M_GICC_IAR / 4] = 61;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, (int)unexpected_hits);
    assert_eq(61, (int)rd(gicc_mock, M_GICC_EOIR));
    /* dispatch-CPUID case (R1-9): CPUID=3 的 SGI 7 走真实 dispatch 路径,
     * handler 只见低 10 位, EOIR mock 必须等于完整 0xC07 */
    assert_eq(0, gic_register_handler(7, probe_handler, 0, "d7"));
    gicc_mock[M_GICC_IAR / 4] = (3u << 10) | 7u;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(7, (int)hit_intid);
    assert_eq(0xC07, (int)rd(gicc_mock, M_GICC_EOIR));
    gic_unregister_handler(35);
    gic_unregister_handler(7);
}

static void suite_sgi(void)
{
    TEST_SUITE("SGIR + set/clear pending");
    mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
    gic_send_sgi(&dev, 5, 0x03, GICD_SGIR_FILTER_LIST);
    assert_eq((int)(((0u << 24) | (3u << 16) | 5u)), (int)rd(gicd_mock, M_GICD_SGIR));
    gic_send_sgi(&dev, 0, 0, GICD_SGIR_FILTER_OTHERS);
    assert_eq((int)(1u << 24), (int)rd(gicd_mock, M_GICD_SGIR));
    gic_send_sgi(&dev, 1, 0x01, GICD_SGIR_FILTER_LIST);   /* AP1→BSP 回发编码 */
    assert_eq((int)((1u << 16) | 1u), (int)rd(gicd_mock, M_GICD_SGIR));
    gic_set_pending(&dev, 40);                        /* ISPENDR 测试注入 (R1-2) */
    assert_eq((uint32_t)(1u << 8), rd(gicd_mock, M_GICD_ISPENDR + 4));
    gic_clear_pending(&dev, 40);                      /* ICPENDR 测试拆除 (R1-2) */
    assert_eq((uint32_t)(1u << 8), rd(gicd_mock, M_GICD_ICPENDR + 4));
}

static void suite_cpu_iface(void)
{
    TEST_SUITE("cpu interface enable");
    mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
    gic_dev_cpu_enable(&dev);
    assert_eq(0xffu, rd(gicc_mock, M_GICC_PMR));
    assert_eq(1u, rd(gicc_mock, M_GICC_CTLR));
}

int main(void)
{
    suite_init(); suite_classify(); suite_enable(); suite_registry();
    suite_ack_eoi(); suite_dispatch(); suite_sgi(); suite_cpu_iface();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_gic_driver",
             __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
