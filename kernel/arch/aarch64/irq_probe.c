/* aarch64 phase 1: GIC dispatch selftest probes (Task 2.2).
 *
 * Two probes drive the dispatch chain end-to-end and prove the
 * entry.S save/restore path is wired correctly:
 *
 *   gic_clobber_probe():
 *     - Self-trigger SGI 2 via GICD_SGIR (filter=SELF, sgi=2).
 *       SGI 2 is enabled for every CPU by gic_cpu_init (banked).
 *     - Set AAPCS64 caller-saved sentinels (x0..x5, x18) BEFORE
 *       the SGI is fired.  The C-side dispatch calls handlers as
 *       `fn(intid, param, regs)` which clobbers x0/x1/x2 (first
 *       three integer args) by AAPCS64 mandate.  x3..x5/x18 are
 *       caller-saved but volatile — used for wider coverage.
 *     - Poll probe_sgi_seen with a 2-second deadline.
 *     - On delivery, verify all sentinels survived → "save-restore OK".
 *     - On clobber → "save-restore FAIL regs=x0-x5,x18".
 *     - On timeout → "save-restore TIMEOUT".
 *
 *   gic_unexpected_probe():
 *     - Install a counting "unexpected" callback.
 *     - Configure SPI 40 (enable + priority 0 + target CPU0).
 *     - Force-pending SPI 40 via GICD_ISPENDR.
 *     - Wait for the callback to fire exactly once.
 *     - Pass = "unexpected intid=40 survived".
 *     - Tear down: disable + clear-pending.
 *
 * Both probes are gated by OS01_SELFTEST so the production kernel
 * is never bloated by them.  Probes MUST run AFTER gic_init +
 * arch_tick_start (handler table populated, IRQ unmasked).
 */

#if OS01_SELFTEST
#include <stdint.h>
#include <stdbool.h>
#include <arch/cpu.h>
#include <arch/aarch64/boot_log.h>
#include <arch/aarch64/gic.h>

#define PROBE_SGI_ID           2u      /* clobber 探针专用自发 SGI */
#define PROBE_UNEXPECTED_INTID 40u

static volatile uint32_t probe_sgi_seen;          /* stlr 置位 / ldar 轮询 */
static volatile uint32_t probe_unexpected_flag;
static volatile uint32_t probe_unexpected_count;
static volatile uint64_t probe_sgir_addr;         /* C 预计算, asm 经 adrp 重取 */
static volatile uint64_t probe_deadline;

static inline void probe_flag_release(volatile uint32_t *p, uint32_t v)
{
    __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory");
}
static inline uint32_t probe_flag_acquire(const volatile uint32_t *p)
{
    uint32_t v;
    __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static void probe_sgi_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    probe_flag_release(&probe_sgi_seen, 1);
}

static void probe_unexpected_cb(uint32_t intid)
{
    kputs("[gic] unexpected IRQ intid=");     /* 与 log_unexpected 同文案 */
    kputu(intid);
    kputs("\n");
    if (intid == PROBE_UNEXPECTED_INTID) {
        probe_unexpected_count += 1;
        probe_flag_release(&probe_unexpected_flag, 1);
    }
}

void gic_clobber_probe(void)
{
    int rc = gic_register_handler(PROBE_SGI_ID, probe_sgi_handler, 0, "probe-sgi");
    if (rc != 0 && rc != -2) {              /* -2 = 已注册, 重复探针沿用 */
        kputs("[gic-probe] save-restore FAIL regs=handler-register\n");
        return;
    }
    probe_sgi_seen = 0;
    probe_sgir_addr = (uint64_t)(uintptr_t)gic_dev_current()->gicd + 0xF00u;
    probe_deadline  = arch_cycle_counter() + arch_cycle_freq() * 2;
    uint64_t bad = 2;
    /* ARM asm notes (the brief's literal code violates two constraints):
     *  - CMP imm must be 0..4095 (#12-bit). Sentinel 0x1111 etc. don't fit;
     *    compare via "mov xN, #sentinel; cmp xS, xN" using a temp.
     *  - MOVK shift must be 0/16/32/48. To build 0x02000002 we use MOVZ +
     *    MOVK with lsl #16: 0x0200_0000 + 0x0002 = 0x02000002. */
    __asm__ __volatile__(
        "mov  x0, #0x1111\n\t"
        "mov  x1, #0x2222\n\t"
        "mov  x2, #0x3333\n\t"
        "mov  x3, #0x4444\n\t"
        "mov  x4, #0x5555\n\t"
        "mov  x5, #0x6666\n\t"
        "mov  x18, #0x7777\n\t"
        "dsb  sy\n\t"
        "adrp x9, probe_sgir_addr\n\t"
        "ldr  x9, [x9, :lo12:probe_sgir_addr]\n\t"
        "mov  x10, #2\n\t"
        "movk x10, #0x200, lsl #16\n\t"    /* x10 = 0x02000002: SGI 2 (bits 0-3) +
                                              SELF filter 0b10 (bits 24-25) */
        "str  w10, [x9]\n\t"                /* ← 自发 IRQ, 立即可入 */
        "1: adrp x9, probe_sgi_seen\n\t"
        "add  x9, x9, :lo12:probe_sgi_seen\n\t"
        "ldar w11, [x9]\n\t"
        "cbnz w11, 4f\n\t"
        "adrp x10, probe_deadline\n\t"
        "ldr  x10, [x10, :lo12:probe_deadline]\n\t"
        "mrs  x11, cntvct_el0\n\t"
        "cmp  x11, x10\n\t"
        "b.lo 1b\n\t"
        "mov  %0, #2\n\t"                   /* 超时: IRQ 未递送/未处理 */
        "b    3f\n"
        "4: mov  x6, #0x1111\n\t cmp x0, x6\n\t b.ne 2f\n\t"
        "mov  x6, #0x2222\n\t cmp x1, x6\n\t b.ne 2f\n\t"
        "mov  x6, #0x3333\n\t cmp x2, x6\n\t b.ne 2f\n\t"
        "mov  x6, #0x4444\n\t cmp x3, x6\n\t b.ne 2f\n\t"
        "mov  x6, #0x5555\n\t cmp x4, x6\n\t b.ne 2f\n\t"
        "mov  x6, #0x6666\n\t cmp x5, x6\n\t b.ne 2f\n\t"
        "mov  x6, #0x7777\n\t cmp x18, x6\n\t b.ne 2f\n\t"
        "mov  %0, #0\n\t"
        "b    3f\n"
        "2: mov  %0, #1\n"
        "3:"
        : "=r"(bad)
        :
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x9", "x10", "x11", "x18",
          "cc", "memory");
    if (bad == 0)      kputs("[gic-probe] save-restore OK\n");
    else if (bad == 1) kputs("[gic-probe] save-restore FAIL regs=x0-x5,x18\n");
    else               kputs("[gic-probe] save-restore TIMEOUT\n");
}

void gic_unexpected_probe(void)
{
    probe_unexpected_count = 0;
    probe_unexpected_flag = 0;
    gic_driver_set_unexpected(probe_unexpected_cb);  /* R2-5: 观察递送本身 */
    if (gic_irq_configure(PROBE_UNEXPECTED_INTID, true, 0x00, 0x01) != 0) {
        kputs("[gic-probe] unexpected intid=40 TIMEOUT\n");
        return;
    }
    gic_force_pending(PROBE_UNEXPECTED_INTID);
    uint64_t deadline = arch_cycle_counter() + arch_cycle_freq() * 2;
    while (probe_flag_acquire(&probe_unexpected_flag) == 0) {  /* 等递送证据 */
        if ((uint64_t)arch_cycle_counter() > deadline) {
            kputs("[gic-probe] unexpected intid=40 TIMEOUT\n");
            gic_irq_configure(PROBE_UNEXPECTED_INTID, false, 0x00, 0x01);
            gic_clear_pending_irq(PROBE_UNEXPECTED_INTID);
            return;
        }
        arch_cpu_pause();
    }
    gic_irq_configure(PROBE_UNEXPECTED_INTID, false, 0x00, 0x01);  /* 拆除 */
    gic_clear_pending_irq(PROBE_UNEXPECTED_INTID);
    if (probe_unexpected_count == 1)
        kputs("[gic-probe] unexpected intid=40 survived\n");     /* 恰一次 */
    else
        kputs("[gic-probe] unexpected intid=40 FAIL count!=1\n");
}
#endif /* OS01_SELFTEST */
