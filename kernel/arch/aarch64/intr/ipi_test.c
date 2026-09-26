/* SGI/IPI 跨核测试（spec §7.5）。
 *
 * 内存序契约（R1-6 + R3-1，与 arch/aarch_percpu.h:93-113 的 boot_online/bench_done
 * 完全同模式）：
 *   AP  : arch_atomic_fetch_add(&ipi_received[cpu], 1)   ← ldxr/stxr 原子计数
 *         ipi_flag_release(&ipi_done[cpu], 1)            ← stlr (RELEASE store)：
 *                                                             计数先于标志可见
 *   BSP : ipi_flag_acquire(&ipi_done[cpu]) == 1           ← ldar (acquire load)：
 *                                                             见标志后读计数必得递增
 * 超时兜底 cntvct deadline（不依赖 IRQ，phase1 spec §2.1 既有方法）。
 * AP handler 不打印（多核并发写 PL011 会绞线, spec §8 R4）。
 *
 * R1-9 回发确认（R2-6 收口）：AP1 收到 SGI 0 后回发 SGI 1
 * (filter=LIST targets=0x01) 给 BSP；BSP 的 SGI-1 handler 在 handler 上下文
 * 读 gic_dbg_last_iar()——per-CPU trace 槽只被本核 ack 写入（同核 dispatch
 * 串行 + IRQ masked 无嵌套，跨核 ack 写各自槽），因此读到的必是本 SGI 的
 * 原始 IAR，无跨核覆写竞态。期望 0x401 = CPUID(1)<<10 | SGI 1——非零
 * CPUID 真实穿越 ack 路径。 */
#if OS01_SELFTEST
#include <stdint.h>
#include <arch/aarch64/gic_pub.h>  /* R3-1/R4-1: arch_publish_handler_table() */
#include <arch/irq.h>
#include <arch/atomic.h>
#include <arch/cpu.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/boot_log.h>
#include <arch/aarch_percpu.h>
#include <percpu/percpu.h>

#define IPI_SGI_ID       0u
#define IPI_REPLY_SGI    1u
#define IPI_WAIT_SECONDS 2u

static volatile uint64_t ipi_received[AARCH64_BOOT_MAX_CPUS];  /* fetch_add 原子递增 */
static volatile uint32_t ipi_done[AARCH64_BOOT_MAX_CPUS];      /* stlr 置 1 / ldar 读 */
static volatile uint32_t bsp_reply_flag;                       /* stlr 置 1 / ldar 读 */
static volatile uint32_t bsp_raw_iar;                          /* SGI-1 handler 捕获 */

static inline void ipi_flag_release(volatile uint32_t *p, uint32_t v)
{
    __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory");
}
static inline uint32_t ipi_flag_acquire(const volatile uint32_t *p)
{
    uint32_t v;
    __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static uint32_t ipi_cpu_id(void)
{
    /* TPIDR_EL1 points at percpu_data[] after percpu_install_gs(). */
    return cpu_id();
}

static void ipi_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    uint32_t cpu = ipi_cpu_id();
    if (cpu >= AARCH64_BOOT_MAX_CPUS) return;
    arch_atomic_fetch_add(&ipi_received[cpu], 1);   /* 原子计数 (atomic.h:47-58) */
    ipi_flag_release(&ipi_done[cpu], 1);            /* RELEASE：计数先于标志 */
    if (cpu == 1)                                   /* AP1 回发 SGI 1 → BSP (R1-9) */
        gic_send_sgi(gic_dev_current(), IPI_REPLY_SGI, 0x01, GICD_SGIR_FILTER_LIST);
}

static void bsp_reply_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    bsp_raw_iar = gic_dbg_last_iar();               /* 本 CPU trace 槽 (R2-6):
                                                       必为本 SGI 的原始 IAR */
    ipi_flag_release(&bsp_reply_flag, 1);
}

void gic_ipi_test(uint32_t cpu_count)
{
    if (cpu_count > AARCH64_BOOT_MAX_CPUS) cpu_count = AARCH64_BOOT_MAX_CPUS;
    for (uint32_t i = 0; i < AARCH64_BOOT_MAX_CPUS; ++i) {
        ipi_received[i] = 0; ipi_done[i] = 0;
    }
    bsp_reply_flag = 0; bsp_raw_iar = 0;
    if (gic_register_handler(IPI_SGI_ID, ipi_handler, 0, "ipi0") != 0 ||
        gic_register_handler(IPI_REPLY_SGI, bsp_reply_handler, 0, "ipi1") != 0) {
        log_err("[ipi] register FAIL\n");
        return;
    }
    /* R3-1：AP 已 secondary_idle 开 DAIF.I 等 SGI；BSP 写 handlers[] 是
     * 普通内存写，Device-nGnRnE 的 GICD_SGIR 写可不与之排序（普通内存→
     * Device 不强制 ordering）。AP 收到 SGI 时 handler 指针/param 可能
     * 尚未可见 → 走 unexpected_cb 路径、不置 ipi_done、致 BSP 超时。
     * 修法：发布屏障 dsb ishst（Inner-Shareable store-only，对所有
     * SGI 唤醒的核可见），调用经 arch 包装 arch_publish_handler_table()
     * 强制未来调用方不遗漏（见 spec §7.5 + §8 R3-1）。注：cntp / pl011
     * / probe-sgi 的注册在 SMP 启动前或本核自触发，**不需要**本屏障——
     * R3 评审要求只在 Task 3.2 路径里加，不进 gic_register_handler。 */
    arch_publish_handler_table();
    kputs("[ipi] send sgi=0 filter=others\n");
    gic_send_sgi(gic_dev_current(), IPI_SGI_ID, 0, GICD_SGIR_FILTER_OTHERS);

    uint64_t deadline = arch_cycle_counter() + arch_cycle_freq() * IPI_WAIT_SECONDS;
    uint32_t done = 0;
    for (;;) {
        done = 0;
        for (uint32_t i = 1; i < cpu_count; ++i)
            if (ipi_flag_acquire(&ipi_done[i]) != 0) ++done;    /* ACQUIRE */
        if (done + 1 >= cpu_count) break;
        if ((uint64_t)arch_cycle_counter() > deadline) break;
        arch_cpu_pause();
    }
    for (uint32_t i = 1; i < cpu_count; ++i) {
        kputs("[ipi] cpu=");
        kputu(i);
        kputs(" received=");
        kputu(ipi_received[i]);
        kputs("\n");
    }
    kputs("[ipi] summary targets=");
    kputu(cpu_count - 1);
    kputs(" received=");
    kputu(done);
    kputs(done + 1 >= cpu_count ? " status=PASS\n" : " status=FAIL\n");

    if (cpu_count >= 2) {                           /* R1-9 回发确认 */
        uint64_t reply_deadline = arch_cycle_counter()
                                + arch_cycle_freq() * IPI_WAIT_SECONDS;
        while (ipi_flag_acquire(&bsp_reply_flag) == 0) {
            if ((uint64_t)arch_cycle_counter() > reply_deadline) break;
            arch_cpu_pause();
        }
        kputs("[ipi] bsp raw_iar=0x");
        kputx(bsp_raw_iar);
        kputs("\n");
    }
}
#endif
