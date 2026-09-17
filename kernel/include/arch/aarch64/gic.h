#ifndef OS01_AARCH64_GIC_H
#define OS01_AARCH64_GIC_H
#include <stdint.h>
#include <stdbool.h>

#define GIC_INTID_SPURIOUS   1023u
#define GIC_INTID_SPI_FIRST  32u
#define GIC_INTID_SPI_LAST   1019u
#define GIC_HANDLER_MAX      1020u          /* 表容量上限（TYPER 再大也截断） */
#define GICD_SGIR_FILTER_LIST   0u
#define GICD_SGIR_FILTER_OTHERS 1u
#define GICD_SGIR_FILTER_SELF   2u

struct pt_regs;                              /* 前向声明：host 编译不进 facade */
typedef void (*gic_handler_fn)(uint32_t intid, uint64_t param, struct pt_regs *regs);

enum gic_irq_type { GIC_IRQ_INVALID = 0, GIC_IRQ_SGI, GIC_IRQ_PPI, GIC_IRQ_SPI };

struct gic_dev {
    volatile uint32_t *gicd;                 /* Device-nGnRnE, identity 映射 */
    volatile uint32_t *gicc;
    uint32_t nr_intids;                      /* (TYPER.ITLinesNumber+1)*32, cap 1020 */
    volatile uint32_t dbg_last_iar;          /* 单槽 trace（hosttest 默认路径）;
                                              内核配 set_cpu_index 后另写 per-CPU 槽 */
};

/* hw 层 —— 全部经 dev 指针；返回 0 成功 / 非 0 失败；不打印 */
int  gic_dev_init(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicc);
void gic_dev_dist_enable(struct gic_dev *dev);    /* GICD_CTLR = 1 */
void gic_dev_dist_disable(struct gic_dev *dev);
void gic_dev_cpu_enable(struct gic_dev *dev);     /* PMR=0xff + GICC_CTLR=1（屏障在 wrapper）*/
enum gic_irq_type gic_irq_type(uint32_t intid);
int  gic_irq_config(struct gic_dev *dev, uint32_t intid, bool enable,
                    uint8_t prio, uint8_t targets); /* IGROUPR0+ISENABLER/ICENABLER
                                                     + IPRIORITYR + ITARGETSR(仅 SPI)
                                                     —— SPI 递送先决条件 (spec §2.2 R1-2) */
int  gic_irq_enable(struct gic_dev *dev, uint32_t intid);
int  gic_irq_disable(struct gic_dev *dev, uint32_t intid);
uint32_t gic_ack(struct gic_dev *dev);            /* 原始 IAR（含 CPUID 位）+ 记录 dbg_last_iar */
void gic_eoi(struct gic_dev *dev, uint32_t iar);  /* 原样写回完整 IAR (spec §2.3/D7) */
void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter);
void gic_set_pending(struct gic_dev *dev, uint32_t intid);   /* ISPENDR — 测试注入 */
void gic_clear_pending(struct gic_dev *dev, uint32_t intid); /* ICPENDR — 测试拆除 (R1-2) */

/* unexpected 回调注入（R1-3：声明在此，定义在 gic_driver.c） */
void gic_driver_set_unexpected(void (*cb)(uint32_t intid));

/* per-CPU IAR trace（R2-6：消除共享单字段的跨核覆写竞态）。
 * wrapper 注入"读本核逻辑 CPU 号"的实现（TPIDR_EL1 槽）；hosttest 注入
 * mock。gic_ack 在 hook 就位时把原始 IAR 写 trace_iar[cpu]（单槽字段照写）；
 * gic_driver_trace_get(cpu) 读指定槽。 */
#define GIC_TRACE_CPUS 8
void gic_driver_set_cpu_index(uint32_t (*fn)(void));
uint32_t gic_driver_trace_get(uint32_t cpu);

/* handler 注册表 —— 模块级全局（Phase 1 非 per-CPU, spec §4.1） */
int  gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                          const char *name);       /* 0 / -1 越界 / -2 已注册 */
int  gic_unregister_handler(uint32_t intid);       /* 0 / -1 越界 / -3 未注册 */
gic_handler_fn gic_get_handler(uint32_t intid, uint64_t *param_out);

/* 通用 dispatch（trap.c 的 el1_irq 是它的三行壳） */
void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs);

/* 生产 wrapper（gic.c）—— 基址来自 DTB；gic_init/gic_cpu_init 兼容旧调用点 */
struct gic_dev *gic_dev_current(void);
int  gic_irq_configure(uint32_t intid, bool enable, uint8_t prio, uint8_t targets);
void gic_force_pending(uint32_t intid);            /* 探针注入 wrapper */
void gic_clear_pending_irq(uint32_t intid);        /* 探针拆除 wrapper (R1-2) */
uint32_t gic_dbg_last_iar(void);                   /* R1-9: handler 上下文读取无竞态 */
void gic_init(void);
void gic_cpu_init(void);
#endif
