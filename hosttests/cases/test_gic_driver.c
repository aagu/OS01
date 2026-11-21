/* hosttests/cases/test_gic_driver.c — GICv2 driver 单元测试（spec §4.1/G1）。
 *
 * 编译【真实生产文件】kernel/arch/aarch64/intr/gic_driver.c（host clang，无修改），
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

#define MMIO_LOG_MAX 128
struct mmio_op {
    bool is_write;
    const volatile uint32_t *addr;
    uint32_t value;
};
static struct mmio_op mmio_log[MMIO_LOG_MAX];
static unsigned mmio_log_count;
static bool drop_pmr_write;
static uint32_t pmr_read_mask = 0xffu;
static unsigned eoir_write_count;

static void mmio_trace_reset(void)
{
    mmio_log_count = 0;
    drop_pmr_write = false;
    pmr_read_mask = 0xffu;
    eoir_write_count = 0;
}

static void mmio_log_push(bool is_write, volatile uint32_t *address, uint32_t value)
{
    if (mmio_log_count < MMIO_LOG_MAX) {
        mmio_log[mmio_log_count].is_write = is_write;
        mmio_log[mmio_log_count].addr = address;
        mmio_log[mmio_log_count].value = value;
        mmio_log_count++;
    }
}

void gic_host_mmio_write(volatile uint32_t *address, uint32_t value)
{
    mmio_log_push(true, address, value);
    if (address == &gicc_mock[M_GICC_EOIR / 4]) {
        eoir_write_count++;
    }
    if (drop_pmr_write && address == &gicc_mock[M_GICC_PMR / 4]) {
        return;
    }
    if (address == &gicc_mock[M_GICC_PMR / 4]) {
        *address = value & pmr_read_mask;
        return;
    }
    *address = value;
}

uint32_t gic_host_mmio_read(volatile uint32_t *address)
{
    uint32_t val = *address;
    mmio_log_push(false, address, val);
    return val;
}

static unsigned count_mmio_range(bool is_write, const uint32_t *base,
                                 uint32_t off_start, uint32_t off_end)
{
    unsigned count = 0;
    for (unsigned i = 0; i < mmio_log_count; i++) {
        if (mmio_log[i].is_write != is_write) continue;
        if (mmio_log[i].addr >= base + off_start / 4u &&
            mmio_log[i].addr < base + off_end / 4u) {
            count++;
        }
    }
    return count;
}

static int find_write_index(const volatile uint32_t *addr, uint32_t value)
{
    for (unsigned i = 0; i < mmio_log_count; i++) {
        if (mmio_log[i].is_write && mmio_log[i].addr == addr &&
            mmio_log[i].value == value) {
            return (int)i;
        }
    }
    return -1;
}

static void mock_reset_typer(uint32_t typer)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[M_GICD_TYPER / 4] = typer;
    gicd_mock[M_GICD_IIDR / 4] = 0x0200143b;   /* QEMU virt GICv2 IIDR 非零 */
    mmio_trace_reset();
}

static void mock_reset(uint32_t typer_itlines)
{
    mock_reset_typer(typer_itlines & 0x1f);
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
    assert_eq(0, gic_dev_cpu_enable(&dev));
    assert_eq(0xffu, rd(gicc_mock, M_GICC_PMR));
    assert_eq(1u, rd(gicc_mock, M_GICC_CTLR));
}

static void suite_security_views_and_pmr(void)
{
    TEST_SUITE("security views, IGROUPR, PMR readback & write order");

    /* 1. Group 0 view: TYPER = 0x001f (SecurityExtn=0, CPUNumber=0 => 1 iface, 1020 intids) */
    mock_reset_typer(0x001fu);
    for (unsigned i = 0; i < 32; i++) {
        gicd_mock[M_GICD_IGROUPR / 4 + i] = 0xffffffffu;
    }
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    assert_false(dev.security_extn);
    assert_eq(1, (int)dev.nr_cpu_interfaces);
    assert_eq(1020, (int)dev.nr_intids);

    mmio_trace_reset();
    gic_dev_dist_enable(&dev);
    assert_eq(0, gic_irq_config(&dev, 33, true, 0x00, 0x01));
    assert_true(count_mmio_range(false, gicd_mock, M_GICD_IGROUPR, M_GICD_IGROUPR + 0x80) >= 1);
    assert_true(count_mmio_range(true, gicd_mock, M_GICD_IGROUPR, M_GICD_IGROUPR + 0x80) >= 1);
    assert_eq(0u, rd(gicd_mock, M_GICD_IGROUPR + 4) & (1u << 1));
    assert_eq(0, gic_dev_cpu_enable(&dev));
    assert_eq(1u, rd(gicd_mock, M_GICD_CTLR));
    assert_eq(1u, rd(gicc_mock, M_GICC_CTLR));
    int pmr_w = find_write_index(&gicc_mock[M_GICC_PMR / 4], 0xffu);
    int ctlr_w = find_write_index(&gicc_mock[M_GICC_CTLR / 4], 1u);
    assert_true(pmr_w >= 0 && ctlr_w >= 0 && pmr_w < ctlr_w);

    /* 2. Group 1 view: TYPER = 0x041f (SecurityExtn=1, CPUNumber=0 => 1 iface, 1020 intids) */
    mock_reset_typer(0x041fu);
    for (unsigned i = 0; i < 32; i++) {
        gicd_mock[M_GICD_IGROUPR / 4 + i] = 0xffffffffu;
    }
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    assert_true(dev.security_extn);
    assert_eq(1, (int)dev.nr_cpu_interfaces);
    assert_eq(1020, (int)dev.nr_intids);

    mmio_trace_reset();
    gic_dev_dist_enable(&dev);
    assert_eq(0, gic_irq_config(&dev, 33, true, 0x00, 0x01));
    assert_eq(0u, count_mmio_range(false, gicd_mock, M_GICD_IGROUPR, M_GICD_IGROUPR + 0x80));
    assert_eq(0u, count_mmio_range(true, gicd_mock, M_GICD_IGROUPR, M_GICD_IGROUPR + 0x80));
    assert_eq(0xffffffffu, rd(gicd_mock, M_GICD_IGROUPR + 4));
    assert_eq(0, gic_dev_cpu_enable(&dev));
    assert_eq(1u, rd(gicd_mock, M_GICD_CTLR));
    assert_eq(1u, rd(gicc_mock, M_GICC_CTLR));
    pmr_w = find_write_index(&gicc_mock[M_GICC_PMR / 4], 0xffu);
    ctlr_w = find_write_index(&gicc_mock[M_GICC_CTLR / 4], 1u);
    assert_true(pmr_w >= 0 && ctlr_w >= 0 && pmr_w < ctlr_w);

    /* SGIR NSATT bit (bit 15) must always be 0 */
    gic_send_sgi(&dev, 5, 0x03, GICD_SGIR_FILTER_LIST);
    assert_eq(0u, rd(gicd_mock, M_GICD_SGIR) & (1u << 15));

    /* 3. Partial NS-visible PMR readback (e.g. 0xf8) succeeds */
    mock_reset_typer(0x047fu); /* SecurityExtn=1, CPUNumber=3 => 4 ifaces */
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    assert_eq(4, (int)dev.nr_cpu_interfaces);
    pmr_read_mask = 0xf8u;
    assert_eq(0, gic_dev_cpu_enable(&dev));
    assert_eq(0xf8u, rd(gicc_mock, M_GICC_PMR));
    assert_eq(1u, rd(gicc_mock, M_GICC_CTLR));

    /* 4. Dropped PMR write / all-zero PMR readback fails (-1) and leaves GICC_CTLR disabled */
    mock_reset_typer(0x041fu);
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    drop_pmr_write = true;
    assert_eq(-1, gic_dev_cpu_enable(&dev));
    assert_eq(0u, rd(gicc_mock, M_GICC_CTLR));

    mock_reset_typer(0x041fu);
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    pmr_read_mask = 0x00u;
    assert_eq(-1, gic_dev_cpu_enable(&dev));
    assert_eq(0u, rd(gicc_mock, M_GICC_CTLR));
}

static void suite_boundaries_and_special_intids(void)
{
    TEST_SUITE("handler boundaries, unused slots, and 1020-1023");
    mock_reset_typer(0x001fu); /* 1020 intids */
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));

    /* Test unused slots across SGI/PPI/SPI up to 1019 */
    const uint32_t test_ids[] = { 5u, 15u, 16u, 29u, 31u, 1019u };
    for (unsigned i = 0; i < sizeof(test_ids) / sizeof(test_ids[0]); i++) {
        uint32_t id = test_ids[i];
        uint64_t expected_param = 0xabc00u + id;
        hit_calls = 0;
        hit_intid = 0xffffffffu;
        hit_param = 0;
        assert_eq(0, gic_register_handler(id, probe_handler, expected_param, "slot"));
        assert_eq(-2, gic_register_handler(id, probe_handler, 0, "dup"));
        gicc_mock[M_GICC_IAR / 4] = id;
        gic_dev_dispatch(&dev, NULL);
        assert_eq(1, (int)hit_calls);
        assert_eq((int)id, (int)hit_intid);
        assert_true(hit_param == expected_param);
        assert_eq((int)id, (int)rd(gicc_mock, M_GICC_EOIR));
        assert_eq(0, gic_unregister_handler(id));
        assert_eq(-3, gic_unregister_handler(id));
        /* Reuse slot */
        assert_eq(0, gic_register_handler(id, probe_handler, expected_param + 1u, "reuse"));
        assert_eq(0, gic_unregister_handler(id));
    }

    /* Raw SGI IAR 0xc05 (CPUID=3, intid=5) */
    assert_eq(0, gic_register_handler(5u, probe_handler, 0x55u, "sgi5"));
    hit_calls = 0;
    gicc_mock[M_GICC_IAR / 4] = 0xc05u;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, (int)hit_calls);
    assert_eq(5, (int)hit_intid);
    assert_eq(0xc05, (int)rd(gicc_mock, M_GICC_EOIR));
    assert_eq(0, gic_unregister_handler(5u));

    /* 1019 unregistered: unexpected callback + EOIR write */
    unexpected_hits = 0;
    unexpected_last = 0;
    gic_driver_set_unexpected(probe_unexpected);
    mmio_trace_reset();
    gicc_mock[M_GICC_IAR / 4] = 1019u;
    gic_dev_dispatch(&dev, NULL);
    assert_eq(1, (int)unexpected_hits);
    assert_eq(1019, (int)unexpected_last);
    assert_eq(1u, eoir_write_count);
    assert_eq(1019, (int)rd(gicc_mock, M_GICC_EOIR));

    /* Special INTIDs 1020-1023: no handler lookup/call, no unexpected, no EOIR */
    for (uint32_t sp = 1020u; sp <= 1023u; sp++) {
        hit_calls = 0;
        unexpected_hits = 0;
        mmio_trace_reset();
        gicc_mock[M_GICC_EOIR / 4] = 0;
        gicc_mock[M_GICC_IAR / 4] = sp;
        gic_dev_dispatch(&dev, NULL);
        assert_eq(0, (int)hit_calls);
        assert_eq(0, (int)unexpected_hits);
        assert_eq(0u, eoir_write_count);
        assert_eq(0u, rd(gicc_mock, M_GICC_EOIR));
    }
    gic_driver_set_unexpected(NULL);

    /* TYPER=0 => nr_intids=32: 31 allowed, 32 rejected */
    mock_reset_typer(0x0000u);
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    assert_eq(32, (int)dev.nr_intids);
    assert_eq(0, gic_register_handler(31u, probe_handler, 0, "ok31"));
    assert_eq(-1, gic_register_handler(32u, probe_handler, 0, "bad32"));
    assert_eq(0, gic_irq_enable(&dev, 31u));
    assert_eq(-1, gic_irq_enable(&dev, 32u));
    assert_eq(0, gic_unregister_handler(31u));
}

int main(void)
{
    suite_init(); suite_classify(); suite_enable(); suite_registry();
    suite_ack_eoi(); suite_dispatch(); suite_sgi(); suite_cpu_iface();
    suite_security_views_and_pmr();
    suite_boundaries_and_special_intids();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_gic_driver",
             __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
