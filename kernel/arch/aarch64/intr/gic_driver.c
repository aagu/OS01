/* GICv2 driver core — 纯逻辑, 无 UART/DTB/asm 依赖（hosttest 可编译, spec §4.1）。
 * 寄存器偏移沿用 kernel/include/arch/aarch64/reg.h 的值, 此处自带同值定义
 * （<arch/aarch64/reg.h> 的 inline 访问器是 target 专用）。 */
#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>
#ifdef AARCH64_AP_SCHED_WAKE_TEST
#include <selftest/aarch64_ap_scheduler.h>
#endif
#ifdef AARCH64_SCHED_TICK_TEST
#include <selftest/aarch64_sched_tick.h>
#endif
#ifdef AARCH64_USER_RUNTIME_TEST
#include <selftest/aarch64_user_runtime.h>
#endif

#define GICD_CTLR       0x000u
#define GICD_TYPER      0x004u
#define GICD_IIDR       0x008u
#define GICD_IGROUPR    0x080u
#define GICD_ISENABLER  0x100u
#define GICD_ICENABLER  0x180u
#define GICD_ISPENDR    0x200u
#define GICD_ICPENDR    0x280u
#define GICD_IPRIORITYR 0x400u
#define GICD_ITARGETSR  0x800u
#define GICD_SGIR       0xF00u
#define GICC_CTLR       0x000u
#define GICC_PMR        0x004u
#define GICC_IAR        0x00Cu
#define GICC_EOIR       0x010u
#define GICR_TYPER      0x008u
#define GICR_WAKER      0x014u
#define GICR_SGI_OFF    0x10000u
#define GICR_IGROUPR0   0x080u
#define GICR_ISENABLER0 0x100u
#define GICR_ICENABLER0 0x180u
#define GICR_IPRIORITYR 0x400u
#define GICR_IGRPMODR0  0xD00u
#define GICR_STRIDE     0x20000u

static inline void w32(volatile uint32_t *m, uint32_t off, uint32_t v)
{
#ifdef OS01_HOST_GIC_MMIO_TRACE
    extern void gic_host_mmio_write(volatile uint32_t *, uint32_t);
    gic_host_mmio_write(m + off / 4u, v);
#else
    m[off / 4u] = v;
#endif
#if defined(AARCH64_AP_SCHED_WAKE_TEST) || defined(AARCH64_SCHED_TICK_TEST)
/* Observe each actual EOIR write, not just a completed dispatch call.
 * In this driver offset 0x10 is written only for the CPU-interface EOI. */
    if (off == GICC_EOIR && (v & 0x3ffu) == 4u) {
        extern uint32_t gic_trace_cpu(void);
#ifdef AARCH64_AP_SCHED_WAKE_TEST
        aarch64_ap_sched_sgi_trace(gic_trace_cpu(), v, true);
#else
        aarch64_sched_tick_sgi_trace(gic_trace_cpu(), v, true);
#endif
    }
#endif
}
static inline uint32_t r32(volatile uint32_t *m, uint32_t off)
{
#ifdef OS01_HOST_GIC_MMIO_READ_TRACE
    extern uint32_t gic_host_mmio_read(volatile uint32_t *);
    return gic_host_mmio_read(m + off / 4u);
#else
    return m[off / 4u];
#endif
}

/* 模块级边界（最近一次 gic_dev_init 写入；handler 表 API 无 dev 指针，
 * 用此全局裁断 intid 范围——suite_registry 期望 intid >= nr_intids 拒绝）。 */
static uint32_t g_nr_intids;

int gic_dev_init(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicc)
{
    if (!dev || !gicd || !gicc) return -1;
    dev->gicd = gicd; dev->gicc = gicc; dev->gicr = 0;
    dev->is_v3 = false; dev->dbg_last_iar = 0;
    if (r32(gicd, GICD_IIDR) == 0) return -1;         /* 镜像 gic.c:30-33 的门 */
    uint32_t typer = r32(gicd, GICD_TYPER);
    uint32_t lines = (typer & 0x1fu) + 1u;
    dev->nr_intids = lines * 32u;
    if (dev->nr_intids > GIC_HANDLER_MAX) dev->nr_intids = GIC_HANDLER_MAX;
    dev->security_extn = ((typer >> 10) & 1u) != 0u;
    dev->nr_cpu_interfaces = (uint8_t)(((typer >> 5) & 7u) + 1u);
    g_nr_intids = dev->nr_intids;                     /* 同步给 handler 表 */
    return 0;
}

int gic_dev_init_v3(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicr)
{
    if (!dev || !gicd || !gicr) return -1;
    dev->gicd = gicd; dev->gicc = 0; dev->gicr = gicr;
    dev->is_v3 = true; dev->dbg_last_iar = 0;
    if (r32(gicd, GICD_IIDR) == 0) return -1;
    uint32_t typer = r32(gicd, GICD_TYPER);
    uint32_t lines = (typer & 0x1fu) + 1u;
    dev->nr_intids = lines * 32u;
    if (dev->nr_intids > GIC_HANDLER_MAX) dev->nr_intids = GIC_HANDLER_MAX;
    dev->security_extn = ((typer >> 10) & 1u) != 0u;
    dev->nr_cpu_interfaces = 8u;
    g_nr_intids = dev->nr_intids;
    return 0;
}

#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
static volatile uint32_t *gicr_find_self(const struct gic_dev *dev)
{
    uint64_t mpidr = 0;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t aff = (uint32_t)(((mpidr >> 8) & 0xff000000ULL) | (mpidr & 0x00ffffffULL));
    volatile uint32_t *rd = dev->gicr;
    for (uint32_t i = 0; i < 64u; ++i) {
        uint32_t typer_lo = r32(rd, GICR_TYPER);
        uint32_t typer_hi = r32(rd, GICR_TYPER + 4u);
        if (typer_hi == aff)
            return rd;
        if (typer_lo & (1u << 4))
            break;
        rd += GICR_STRIDE / 4u;
    }
    return 0;
}
#endif

void gic_dev_dist_enable(struct gic_dev *dev)
{
    if (!dev) return;
    if (dev->is_v3) {
        w32(dev->gicd, GICD_CTLR, (1u << 4) | (1u << 1)); /* ARE_NS | EnableGrp1NS */
        for (uint32_t i = 0; i < 100000u; ++i) {
            if ((r32(dev->gicd, GICD_CTLR) & (1u << 31)) == 0u)
                break;
        }
        return;
    }
    w32(dev->gicd, GICD_CTLR, 1u);
}

void gic_dev_dist_disable(struct gic_dev *dev)
{
    if (!dev) return;
    w32(dev->gicd, GICD_CTLR, 0u);
}

enum gic_irq_type gic_irq_type(uint32_t intid)
{
    if (intid <= 15u)  return GIC_IRQ_SGI;
    if (intid <= 31u)  return GIC_IRQ_PPI;
    if (intid <= GIC_INTID_SPI_LAST) return GIC_IRQ_SPI;
    return GIC_IRQ_INVALID;
}

static int intid_ok(const struct gic_dev *dev, uint32_t intid)
{
    return gic_irq_type(intid) != GIC_IRQ_INVALID && intid < dev->nr_intids;
}

int gic_irq_config(struct gic_dev *dev, uint32_t intid, bool enable,
                   uint8_t prio, uint8_t targets)
{
    if (!dev || !intid_ok(dev, intid)) return -1;
    if (dev->is_v3) {
        uint32_t pr_off = GICR_IPRIORITYR + (intid / 4u) * 4u;
        uint32_t shift = (intid % 4u) * 8u;
        if (intid < 32u) {
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
            volatile uint32_t *rd = gicr_find_self(dev);
            if (!rd) return -1;
            volatile uint32_t *sgi = rd + (GICR_SGI_OFF / 4u);
            w32(sgi, GICR_IGROUPR0, r32(sgi, GICR_IGROUPR0) | (1u << intid));
            w32(sgi, pr_off, (r32(sgi, pr_off) & ~(0xffu << shift)) | ((uint32_t)prio << shift));
            if (enable) w32(sgi, GICR_ISENABLER0, 1u << intid);
            else        w32(sgi, GICR_ICENABLER0, 1u << intid);
#endif
            return 0;
        }
        volatile uint32_t *g = dev->gicd;
        uint32_t grp_off = GICD_IGROUPR + (intid / 32u) * 4u;
        w32(g, grp_off, r32(g, grp_off) | (1u << (intid % 32u)));
        w32(g, GICD_IPRIORITYR + (intid / 4u) * 4u,
            (r32(g, GICD_IPRIORITYR + (intid / 4u) * 4u) & ~(0xffu << shift)) | ((uint32_t)prio << shift));
        if (enable) w32(g, GICD_ISENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
        else        w32(g, GICD_ICENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
        return 0;
    }
    volatile uint32_t *g = dev->gicd;
    /* 无 Security Extensions 时配置 Group 0；有 Security Extensions 时
     * Non-secure EL1 由固件提供 Group 1，跳过 IGROUPR 访问。 */
    if (!dev->security_extn) {
        uint32_t grp_off = GICD_IGROUPR + (intid / 32u) * 4u;
        w32(g, grp_off, r32(g, grp_off) & ~(1u << (intid % 32u)));
    }
    /* 优先级字节（SGI/PPI 的 banked 域同样可写） */
    uint32_t pr_off = GICD_IPRIORITYR + (intid / 4u) * 4u;
    uint32_t shift = (intid % 4u) * 8u;
    w32(g, pr_off, (r32(g, pr_off) & ~(0xffu << shift)) | ((uint32_t)prio << shift));
    /* SPI 路由；SGI/PPI 的 ITARGETSR 只读 banked, 不写。
     * R1-2: SPI 递送先决条件 = enable(ITARGETSR 后的 ISENABLER) + route。 */
    if (gic_irq_type(intid) == GIC_IRQ_SPI) {
        uint32_t tg_off = GICD_ITARGETSR + (intid / 4u) * 4u;
        w32(g, tg_off, (r32(g, tg_off) & ~(0xffu << shift))
                       | ((uint32_t)targets << shift));
    }
    if (enable) w32(g, GICD_ISENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
    else        w32(g, GICD_ICENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
    return 0;
}

int gic_irq_enable(struct gic_dev *dev, uint32_t intid)
{ return gic_irq_config(dev, intid, true, 0x00, 0x01); }

int gic_irq_disable(struct gic_dev *dev, uint32_t intid)
{
    if (!dev || !intid_ok(dev, intid)) return -1;
    w32(dev->gicd, GICD_ICENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
    return 0;
}

int gic_dev_cpu_enable(struct gic_dev *dev)
{
    if (!dev) return -1;
    if (dev->is_v3) {
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
        volatile uint32_t *rd = gicr_find_self(dev);
        if (!rd) return -1;
        uint32_t waker = r32(rd, GICR_WAKER);
        w32(rd, GICR_WAKER, waker & ~(1u << 1));
        for (uint32_t i = 0; i < 100000u; ++i) {
            if ((r32(rd, GICR_WAKER) & (1u << 2)) == 0u)
                break;
        }
        volatile uint32_t *sgi = rd + (GICR_SGI_OFF / 4u);
        w32(sgi, GICR_IGROUPR0, 0xffffffffu);
        w32(sgi, GICR_IGRPMODR0, 0u);
        uint64_t sre = 0;
        __asm__ __volatile__("mrs %0, S3_0_C12_C12_5" : "=r"(sre));
        sre |= 1ULL;
        __asm__ __volatile__("msr S3_0_C12_C12_5, %0\n\tisb" :: "r"(sre));
        __asm__ __volatile__("msr S3_0_C4_C6_0, %0\n\t"
                             "msr S3_0_C12_C12_3, %1\n\t"
                             "msr S3_0_C12_C12_7, %2\n\tisb"
                             :: "r"(UINT64_C(0xff)), "r"(UINT64_C(0)), "r"(UINT64_C(1)));
#endif
        return 0;
    }
    if (!dev->gicc) return -1;
    w32(dev->gicc, GICC_PMR, 0xffu);
    if ((r32(dev->gicc, GICC_PMR) & 0xffu) == 0u) {
        return -1;
    }
    w32(dev->gicc, GICC_CTLR, 1u);
    return 0;
}

uint8_t gic_cpu_target_mask(const struct gic_dev *dev, bool single_cpu_topology)
{
    if (!dev || !dev->gicd) return 0;
    uint8_t mask = 0;
    for (uint32_t w = 0; w < 8u; w++) {
        uint32_t word = r32(dev->gicd, GICD_ITARGETSR + w * 4u);
        for (uint32_t b = 0; b < 4u; b++) {
            uint8_t val = (uint8_t)((word >> (b * 8u)) & 0xffu);
            if (val == 0u) continue;
            if ((val & (uint8_t)(val - 1u)) != 0u) return 0;
            if (mask == 0u) {
                mask = val;
            } else if (mask != val) {
                return 0;
            }
        }
    }
    if (mask == 0u) {
        if (single_cpu_topology && dev->nr_cpu_interfaces == 1u) {
            return 1u;
        }
        return 0;
    }
    return mask;
}

/* per-CPU IAR trace (R2-6)：跨核 ack 只写本 CPU 槽——BSP 在 handler 上下文
 * 读到的必是本核最近一次 ack（同核 dispatch 串行 + IRQ masked, 无嵌套），
 * AP 的 ack 写各自槽，互不覆写。hosttest 用 mock hook 覆盖。 */
static uint32_t (*cpu_index_fn)(void);
static uint64_t (*mpidr_lookup_fn)(uint32_t);
static volatile uint32_t trace_iar[GIC_TRACE_CPUS];

void gic_driver_set_cpu_index(uint32_t (*fn)(void))
{ cpu_index_fn = fn; }

void gic_driver_set_mpidr_lookup(uint64_t (*fn)(uint32_t cpu))
{ mpidr_lookup_fn = fn; }

uint32_t gic_driver_trace_get(uint32_t cpu)
{ return cpu < GIC_TRACE_CPUS ? trace_iar[cpu] : 0u; }

uint32_t gic_ack(struct gic_dev *dev)
{
    uint32_t iar = 1023u;
    if (dev->is_v3) {
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
        uint64_t v = 0;
        __asm__ __volatile__("mrs %0, S3_0_C12_C12_0" : "=r"(v));
        iar = (uint32_t)v;
#endif
    } else {
        iar = r32(dev->gicc, GICC_IAR);
    }
    dev->dbg_last_iar = iar;                /* 单槽 trace（hosttest 默认路径） */
    if (cpu_index_fn) {
        uint32_t c = cpu_index_fn();
        if (c < GIC_TRACE_CPUS) trace_iar[c] = iar;
    }
    return iar;
}

void gic_eoi(struct gic_dev *dev, uint32_t iar)
{
    if (dev->is_v3) {
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
        __asm__ __volatile__("msr S3_0_C12_C12_1, %0\n\tisb" :: "r"((uint64_t)iar));
#endif
        return;
    }
    w32(dev->gicc, GICC_EOIR, iar);         /* 完整 IAR 原样写回 — D7 修复 */
}

#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
static void gicv3_write_sgi1r(uint32_t sgi, uint64_t mpidr)
{
    uint64_t aff3 = (mpidr >> 32) & 0xffULL;
    uint64_t aff2 = (mpidr >> 16) & 0xffULL;
    uint64_t aff1 = (mpidr >> 8) & 0xffULL;
    uint64_t aff0 = mpidr & 0xfULL;
    uint64_t val = (aff3 << 48) | (aff2 << 32) | (((uint64_t)(sgi & 0xfu)) << 24) |
                   (aff1 << 16) | (UINT64_C(1) << aff0);
    __asm__ __volatile__("msr S3_0_C12_C11_5, %0\n\tisb" :: "r"(val));
}
#endif

void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter)
{
    if (dev->is_v3) {
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
        if (filter == GICD_SGIR_FILTER_OTHERS) {
            uint64_t val = (UINT64_C(1) << 40) | (((uint64_t)(sgi & 0xfu)) << 24);
            __asm__ __volatile__("msr S3_0_C12_C11_5, %0\n\tisb" :: "r"(val));
            return;
        }
        if (filter == GICD_SGIR_FILTER_SELF) {
            uint64_t mpidr = 0;
            __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
            gicv3_write_sgi1r(sgi, mpidr);
            return;
        }
        for (uint32_t cpu = 0; cpu < 8u; ++cpu) {
            if ((targets & (1u << cpu)) == 0u)
                continue;
            uint64_t mpidr = mpidr_lookup_fn ? mpidr_lookup_fn(cpu) : (uint64_t)cpu;
            if (mpidr != UINT64_MAX)
                gicv3_write_sgi1r(sgi, mpidr);
        }
#endif
        return;
    }
    w32(dev->gicd, GICD_SGIR,
        ((uint32_t)(filter & 3u) << 24) | ((uint32_t)targets << 16) | (sgi & 0xfu));
}

void gic_set_pending(struct gic_dev *dev, uint32_t intid)
{ w32(dev->gicd, GICD_ISPENDR + (intid / 32u) * 4u, 1u << (intid % 32u)); }

void gic_clear_pending(struct gic_dev *dev, uint32_t intid)
{ w32(dev->gicd, GICD_ICPENDR + (intid / 32u) * 4u, 1u << (intid % 32u)); }

struct gic_handler_slot {
    gic_handler_fn fn;
    uint64_t param;
    const char *name;
};
static struct gic_handler_slot handlers[GIC_HANDLER_MAX];

static void (*unexpected_cb)(uint32_t) = 0;

void gic_driver_set_unexpected(void (*cb)(uint32_t intid))   /* R1-3: 定义在此 */
{ unexpected_cb = cb; }

int gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                         const char *name)
{
    if (gic_irq_type(intid) == GIC_IRQ_INVALID ||
        intid >= GIC_HANDLER_MAX || intid >= g_nr_intids || !fn)
        return -1;
    if (handlers[intid].fn) return -2;
    handlers[intid].fn = fn; handlers[intid].param = param; handlers[intid].name = name;
    return 0;
}

int gic_unregister_handler(uint32_t intid)
{
    if (gic_irq_type(intid) == GIC_IRQ_INVALID ||
        intid >= GIC_HANDLER_MAX || intid >= g_nr_intids) return -1;
    if (!handlers[intid].fn) return -3;
    handlers[intid].fn = 0; return 0;
}

gic_handler_fn gic_get_handler(uint32_t intid, uint64_t *param_out)
{
    if (gic_irq_type(intid) == GIC_IRQ_INVALID ||
        intid >= GIC_HANDLER_MAX || intid >= g_nr_intids)
        return 0;
    if (param_out) *param_out = handlers[intid].param;
    return handlers[intid].fn;
}

void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs)
{
    uint32_t iar = gic_ack(dev);
    uint32_t intid = iar & 0x3ffu;
    if (intid >= 1020u) return;                       /* 1020-1023 绝不查表、绝不写 EOIR */
#ifdef AARCH64_USER_RUNTIME_CASE_MIGRATION
    aarch64_user_mig_note_irq(intid);
#endif
#if defined(AARCH64_AP_SCHED_WAKE_TEST) || defined(AARCH64_SCHED_TICK_TEST)
    if (intid == 4u) {
        extern uint32_t gic_trace_cpu(void);
#ifdef AARCH64_AP_SCHED_WAKE_TEST
        aarch64_ap_sched_sgi_trace(gic_trace_cpu(), iar, false);
#else
        aarch64_sched_tick_sgi_trace(gic_trace_cpu(), iar, false);
#endif
    }
#endif
    uint64_t param = 0;
    gic_handler_fn fn = gic_get_handler(intid, &param);
    if (fn) fn(intid, param, regs);                   /* 设备清源在 EOI 前 */
    else if (unexpected_cb) unexpected_cb(intid);     /* 对齐 time.c:105-115 语义 */
    gic_eoi(dev, iar);                                /* 完整 IAR 回写 (D7) */
#ifdef AARCH64_USER_RUNTIME_TEST
    if (intid == 30u)
        aarch64_user_runtime_test_timer_eoi(intid, regs);
#endif
}
