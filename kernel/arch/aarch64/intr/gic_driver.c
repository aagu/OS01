/* GICv2 driver core — 纯逻辑, 无 UART/DTB/asm 依赖（hosttest 可编译, spec §4.1）。
 * 寄存器偏移沿用 kernel/include/arch/aarch64/reg.h 的值, 此处自带同值定义
 * （<arch/aarch64/reg.h> 的 inline 访问器是 target 专用）。 */
#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>

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

static inline void w32(volatile uint32_t *m, uint32_t off, uint32_t v)
{ m[off / 4u] = v; }
static inline uint32_t r32(volatile uint32_t *m, uint32_t off)
{ return m[off / 4u]; }

/* 模块级边界（最近一次 gic_dev_init 写入；handler 表 API 无 dev 指针，
 * 用此全局裁断 intid 范围——suite_registry 期望 intid >= nr_intids 拒绝）。 */
static uint32_t g_nr_intids;

int gic_dev_init(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicc)
{
    if (!dev || !gicd || !gicc) return -1;
    dev->gicd = gicd; dev->gicc = gicc; dev->dbg_last_iar = 0;
    if (r32(gicd, GICD_IIDR) == 0) return -1;         /* 镜像 gic.c:30-33 的门 */
    uint32_t lines = (r32(gicd, GICD_TYPER) & 0x1fu) + 1u;
    dev->nr_intids = lines * 32u;
    if (dev->nr_intids > GIC_HANDLER_MAX) dev->nr_intids = GIC_HANDLER_MAX;
    g_nr_intids = dev->nr_intids;                     /* 同步给 handler 表 */
    return 0;
}

void gic_dev_dist_enable(struct gic_dev *dev)
{
    if (!dev) return;
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
    volatile uint32_t *g = dev->gicd;
    /* 组 0（secure；QEMU 无 EL3, 沿用 gic.c:16 的清位做法） */
    uint32_t grp_off = GICD_IGROUPR + (intid / 32u) * 4u;
    w32(g, grp_off, r32(g, grp_off) & ~(1u << (intid % 32u)));
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

void gic_dev_cpu_enable(struct gic_dev *dev)
{
    w32(dev->gicc, GICC_PMR, 0xffu);
    w32(dev->gicc, GICC_CTLR, 1u);
}

/* per-CPU IAR trace (R2-6)：跨核 ack 只写本 CPU 槽——BSP 在 handler 上下文
 * 读到的必是本核最近一次 ack（同核 dispatch 串行 + IRQ masked, 无嵌套），
 * AP 的 ack 写各自槽，互不覆写。hosttest 用 mock hook 覆盖。 */
static uint32_t (*cpu_index_fn)(void);
static volatile uint32_t trace_iar[GIC_TRACE_CPUS];

void gic_driver_set_cpu_index(uint32_t (*fn)(void))
{ cpu_index_fn = fn; }

uint32_t gic_driver_trace_get(uint32_t cpu)
{ return cpu < GIC_TRACE_CPUS ? trace_iar[cpu] : 0u; }

uint32_t gic_ack(struct gic_dev *dev)
{
    uint32_t iar = r32(dev->gicc, GICC_IAR);
    dev->dbg_last_iar = iar;                /* 单槽 trace（hosttest 默认路径） */
    if (cpu_index_fn) {
        uint32_t c = cpu_index_fn();
        if (c < GIC_TRACE_CPUS) trace_iar[c] = iar;
    }
    return iar;
}

void gic_eoi(struct gic_dev *dev, uint32_t iar)
{ w32(dev->gicc, GICC_EOIR, iar); }        /* 完整 IAR 原样写回 — D7 修复 */

void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter)
{
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
    if (intid == GIC_INTID_SPURIOUS) return;          /* 绝不写 EOIR */
    uint64_t param = 0;
    gic_handler_fn fn = gic_get_handler(intid, &param);
    if (fn) fn(intid, param, regs);                   /* 设备清源在 EOI 前 */
    else if (unexpected_cb) unexpected_cb(intid);     /* 对齐 time.c:105-115 语义 */
    gic_eoi(dev, iar);                                /* 完整 IAR 回写 (D7) */
}
