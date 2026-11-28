/* @os01-test
 * id: test_aarch64_rp1_console
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: nightly
 * link: rp1-console
 * scenarios: [intr.aarch64.rp1_console]
 */
#include "test_framework.h"
#include <arch/console.h>
#include <arch/aarch64/console.h>
#include <arch/aarch64/uart.h>
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/rp1_irq.h>
#include <arch/aarch64/gic.h>
#include <core/bootinfo.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ── Fake UART + RP1 + GIC + TTY ──────────────────────────────────────── */
static uint32_t g_uart_imsc;
static uint32_t g_uart_icr_writes;
static uint8_t g_rx_fifo[64];
static unsigned g_rx_head, g_rx_tail;
static bool g_drop_imsc_enable;
static bool g_drop_imsc_mask;
static bool g_push_late_byte_on_enable;

static char g_tty_buf[128];
static unsigned g_tty_len;
static struct tty_struct *g_fake_tty = (struct tty_struct *)0x1234;

static struct aarch64_rp1_irq_desc g_irq_desc;
static struct aarch64_rp1_irq_platform g_plat;
static bool g_plat_ready;
static int g_prepare_rc;
static int g_commit_rc;
static int g_abort_rc;
static unsigned g_abort_calls;
static bool g_last_abort_mask_verified;
static rp1_irq_handler_fn g_rp1_cb;
static void *g_rp1_arg;
static bool g_rp1_committed;
static bool g_rp1_quarantined;
static struct rp1_irq *g_fake_rp1_irq = (struct rp1_irq *)0x5678;

static uint32_t g_dtb_spi;
static uint8_t g_bsp_target = 0x01u;
static int g_gic_reg_rc;
static int g_gic_cfg_rc;
static unsigned g_gic_reg_calls;
static unsigned g_gic_unreg_calls;
static gic_handler_fn g_gic_cb;

static uint64_t g_now_ticks;

int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

uint64_t dtb_pl011_base(void) { return UINT64_C(0x1f00030000); }
uint32_t dtb_pl011_spi(void) { return g_dtb_spi; }
uint32_t dtb_pl011_clock(void) { return 48000000u; }
int aarch64_acpi_parse(uint64_t rsdp, void *out) { (void)rsdp; (void)out; return -1; }
bool aarch64_boot_console_firmware_configured(void) { return true; }
uint64_t aarch64_boot_early_console_pa(void) { return UINT64_C(0x1f00030000); }
void aarch64_geni_uart_putc(volatile uint32_t *base, char c) { (void)base; (void)c; }

static void fifo_push(uint8_t c)
{
    g_rx_fifo[g_rx_tail % 64u] = c;
    g_rx_tail++;
}

uint32_t pl011_host_r32(uint32_t off)
{
    if (off == 0x00u) { /* DR */
        if (g_rx_head < g_rx_tail)
            return g_rx_fifo[(g_rx_head++) % 64u];
        return 0u;
    }
    if (off == 0x18u) { /* FR */
        return (g_rx_head < g_rx_tail) ? 0u : (1u << 4);
    }
    if (off == 0x38u) /* IMSC */
        return g_uart_imsc;
    return 0u;
}

void pl011_host_w32(uint32_t off, uint32_t v)
{
    if (off == 0x38u) { /* IMSC */
        if (v == 0x50u && g_drop_imsc_enable)
            return;
        if (v == 0u && g_drop_imsc_mask)
            return;
        g_uart_imsc = v;
        if (v == 0x50u && g_push_late_byte_on_enable) {
            g_push_late_byte_on_enable = false;
            fifo_push('L');
        }
    } else if (off == 0x44u) { /* ICR */
        g_uart_icr_writes++;
    }
}

void write_serial_unlocked(char c) { (void)c; }

void tty_push_input(struct tty_struct *tty, char c)
{
    if (tty == g_fake_tty && g_tty_len < sizeof(g_tty_buf))
        g_tty_buf[g_tty_len++] = c;
}

uint64_t rp1_console_test_now(void)
{
    g_now_ticks += 54000ULL; /* 1 ms per poll */
    return g_now_ticks;
}
uint64_t rp1_console_test_freq(void) { return 54000000ULL; }

const struct aarch64_rp1_irq_desc *aarch64_rp1_irq_desc_get(void) { return &g_irq_desc; }
const struct aarch64_rp1_irq_platform *aarch64_rp1_irq_platform_get(void)
{
    return g_plat_ready ? &g_plat : NULL;
}

int rp1_irq_prepare(uint32_t source, enum rp1_irq_trigger trigger,
                    rp1_irq_handler_fn fn, void *arg,
                    struct rp1_irq **out)
{
    (void)source; (void)trigger;
    if (g_prepare_rc != 0) {
        *out = NULL;
        return g_prepare_rc;
    }
    g_rp1_cb = fn;
    g_rp1_arg = arg;
    *out = g_fake_rp1_irq;
    return 0;
}

int rp1_irq_commit(struct rp1_irq *irq)
{
    (void)irq;
    if (g_commit_rc != 0)
        return g_commit_rc;
    g_rp1_committed = true;
    return 0;
}

int rp1_irq_abort(struct rp1_irq *irq, bool device_mask_verified)
{
    (void)irq;
    g_abort_calls++;
    g_last_abort_mask_verified = device_mask_verified;
    if (!device_mask_verified || g_abort_rc != 0) {
        g_rp1_quarantined = true;
        return -EIO;
    }
    g_rp1_cb = NULL;
    return 0;
}

uint32_t rp1_irq_intid(const struct rp1_irq *irq)
{
    (void)irq;
    return 160u;
}

uint8_t gic_target_bit_get(uint32_t cpu) { return cpu == 0 ? g_bsp_target : 0u; }

int gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param, const char *name)
{
    (void)intid; (void)param; (void)name;
    g_gic_reg_calls++;
    if (g_gic_reg_rc != 0)
        return g_gic_reg_rc;
    g_gic_cb = fn;
    return 0;
}

int gic_unregister_handler(uint32_t intid)
{
    (void)intid;
    g_gic_unreg_calls++;
    g_gic_cb = NULL;
    return 0;
}

int gic_irq_configure(uint32_t intid, bool enable, uint8_t prio, uint8_t targets)
{
    (void)intid; (void)enable; (void)prio; (void)targets;
    return g_gic_cfg_rc;
}

static void reset_env(void)
{
    g_uart_imsc = 0;
    g_uart_icr_writes = 0;
    g_rx_head = g_rx_tail = 0;
    g_drop_imsc_enable = false;
    g_drop_imsc_mask = false;
    g_push_late_byte_on_enable = false;
    memset(g_tty_buf, 0, sizeof(g_tty_buf));
    g_tty_len = 0;

    memset(&g_irq_desc, 0, sizeof(g_irq_desc));
    g_irq_desc.valid = true;
    g_irq_desc.uart_valid = true;
    g_irq_desc.uart_source = 25u;
    memset(&g_plat, 0, sizeof(g_plat));
    g_plat.valid = true;
    g_plat_ready = true;
    g_prepare_rc = 0;
    g_commit_rc = 0;
    g_abort_rc = 0;
    g_abort_calls = 0;
    g_last_abort_mask_verified = false;
    g_rp1_cb = NULL;
    g_rp1_arg = NULL;
    g_rp1_committed = false;
    g_rp1_quarantined = false;

    g_dtb_spi = 0;
    g_bsp_target = 0x01u;
    g_gic_reg_rc = 0;
    g_gic_cfg_rc = 0;
    g_gic_reg_calls = 0;
    g_gic_unreg_calls = 0;
    g_gic_cb = NULL;
    g_now_ticks = 0;

    extern void aarch64_console_test_reset(void);
    aarch64_console_test_reset();
}

static void make_rp1_ctx(struct boot_context *ctx, bool v4)
{
    boot_context_init(ctx);
    ctx->flags = BOOT_CONTEXT_HAS_DTB | BOOT_CONTEXT_HAS_EARLY_MMIO |
                 BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED;
    ctx->early_mmio.console_base = UINT64_C(0x1f00030000);
    ctx->early_mmio.device_base = UINT64_C(0x1000000000);
    ctx->early_mmio.device_blocks = 1u;
    if (v4) {
        ctx->version = BOOT_CONTEXT_VERSION_RP1_PCI;
        ctx->size = sizeof(*ctx);
        ctx->flags |= BOOT_CONTEXT_HAS_RP1_PCI;
    }
}

static void test_rp1_console_irq_and_fallbacks(void)
{
    struct boot_context ctx;

    /* 1. Mode detection: RP1 v4, RP1 v3, normal DTB, ACPI Q8B */
    make_rp1_ctx(&ctx, true);
    assert_true(aarch64_console_uses_rp1_header(&ctx));
    make_rp1_ctx(&ctx, false);
    assert_true(aarch64_console_uses_rp1_header(&ctx));
    ctx.flags &= ~BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED;
    assert_false(aarch64_console_uses_rp1_header(&ctx));
    ctx.flags = BOOT_CONTEXT_HAS_ACPI | BOOT_CONTEXT_HAS_EARLY_MMIO |
                BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED;
    assert_false(aarch64_console_uses_rp1_header(&ctx));

    /* 2. RP1 IRQ arming success: IMSC=0x50, pre-existing + late byte + sparse byte, poll=0 */
    reset_env();
    make_rp1_ctx(&ctx, true);
    aarch64_console_detect_mode(&ctx);
    fifo_push('A'); /* already in FIFO at arming */
    g_push_late_byte_on_enable = true; /* arrives at enable */
    arch_console_input_attach((struct tty_struct *)g_fake_tty);
    assert_eq(0x50u, g_uart_imsc);
    assert_true(g_rp1_committed);
    /* Polling must be disabled */
    aarch64_console_poll();
    assert_eq(0u, g_tty_len);
    /* RP1 ISR drains FIFO without writing full ICR */
    g_rp1_cb(g_rp1_arg);
    fifo_push('S'); /* sparse byte */
    g_rp1_cb(g_rp1_arg);
    assert_eq(3u, g_tty_len);
    assert_eq('A', g_tty_buf[0]);
    assert_eq('L', g_tty_buf[1]);
    assert_eq('S', g_tty_buf[2]);
    assert_eq(0u, g_uart_icr_writes);

    /* 3. IMSC enable readback fails -> mask + abort(true) + poll fallback */
    reset_env();
    make_rp1_ctx(&ctx, true);
    aarch64_console_detect_mode(&ctx);
    g_drop_imsc_enable = true;
    arch_console_input_attach((struct tty_struct *)g_fake_tty);
    assert_eq(1u, g_abort_calls);
    assert_true(g_last_abort_mask_verified);
    assert_false(g_rp1_quarantined);
    fifo_push('P');
    aarch64_console_poll();
    assert_eq(1u, g_tty_len);
    assert_eq('P', g_tty_buf[0]);

    /* 4. Commit fails & IMSC mask readback fails -> abort(false) quarantines + poll works */
    reset_env();
    make_rp1_ctx(&ctx, true);
    aarch64_console_detect_mode(&ctx);
    g_commit_rc = -1;
    g_drop_imsc_mask = true;
    arch_console_input_attach((struct tty_struct *)g_fake_tty);
    assert_eq(1u, g_abort_calls);
    assert_false(g_last_abort_mask_verified);
    assert_true(g_rp1_quarantined);
    fifo_push('Q');
    aarch64_console_poll();
    assert_eq(1u, g_tty_len);
    assert_eq('Q', g_tty_buf[0]);

    /* 5. RP1 v3 without provider -> poll fallback */
    reset_env();
    make_rp1_ctx(&ctx, false);
    g_plat_ready = false;
    aarch64_console_detect_mode(&ctx);
    arch_console_input_attach((struct tty_struct *)g_fake_tty);
    fifo_push('V');
    aarch64_console_poll();
    assert_eq(1u, g_tty_len);
    assert_eq('V', g_tty_buf[0]);

    /* 6. Normal DTB direct SPI + handler revoked on gic_irq_configure failure */
    reset_env();
    boot_context_init(&ctx);
    ctx.flags = BOOT_CONTEXT_HAS_DTB;
    aarch64_console_detect_mode(&ctx);
    g_dtb_spi = 33u;
    g_gic_cfg_rc = -1;
    arch_console_input_attach((struct tty_struct *)g_fake_tty);
    assert_eq(1u, g_gic_reg_calls);
    assert_eq(1u, g_gic_unreg_calls);
    fifo_push('D');
    aarch64_console_poll();
    assert_eq(1u, g_tty_len);
    assert_eq('D', g_tty_buf[0]);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_rp1_console_irq_and_fallbacks),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 RP1 console IRQ attach");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
