/* kernel/arch/aarch64/intr/rp1_irq.c — Reusable RP1 MSI-X interrupt service & delivery probe */
#include <arch/aarch64/rp1_irq.h>
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/bcm2712_mip.h>
#include <arch/aarch64/gic.h>
#include <device/boot.h>
#include <errno.h>
#include <log/log.h>
#include <stddef.h>
#include <string.h>

#define RP1_APB_IRQ_BASE      0x08U
#define RP1_APB_IRQ_SET_OFF   0x800U
#define RP1_APB_IRQ_CLR_OFF   0xc00U
#define RP1_APB_BIT_ENABLE    (1U << 0)
#define RP1_APB_BIT_TEST      (1U << 1)
#define RP1_APB_BIT_IACK      (1U << 2)
#define RP1_APB_BIT_IACK_EN   (1U << 3)

#define RP1_MSIX_CTL_ENABLE   0x8000U
#define RP1_MSIX_CTL_FMASK    0x4000U
#define RP1_MSIX_VEC_MASK     0x0001U

#define RP1_USBHOST_SRC_MIN   30U
#define RP1_USBHOST_SRC_MAX   39U

#define MIP_INT_MASKL_HOST    0x40U

#ifdef OS01_HOST_TEST
uint64_t rp1_irq_test_now(void);
uint64_t rp1_irq_test_freq(void);
#define rp1_counter_now()  rp1_irq_test_now()
#define rp1_counter_freq() rp1_irq_test_freq()
#else
#include <arch/aarch64/reg.h>
#define rp1_counter_now()  cntpct_el0()
#define rp1_counter_freq() cntfrq_el0()
#endif

enum rp1_fn_state {
    RP1_FN_UNCLAIMED = 0,
    RP1_FN_PREPARING = 1,
    RP1_FN_OWNED = 2,
    RP1_FN_QUARANTINED = 3,
};

enum rp1_slot_state {
    RP1_SLOT_FREE = 0,
    RP1_SLOT_PREPARING = 1,
    RP1_SLOT_COMMITTED = 2,
    RP1_SLOT_QUARANTINED = 3,
};

struct rp1_irq {
    uint32_t source;
    enum rp1_irq_trigger trigger;
    rp1_irq_handler_fn fn;
    void *arg;
    enum rp1_slot_state state;
    struct bcm2712_mip_route mip_route;
    uint32_t saved_tbl_lo;
    uint32_t saved_tbl_hi;
    uint32_t saved_tbl_data;
    uint32_t saved_tbl_ctrl;
    uint32_t saved_apb_cfg;
    bool table_written;
    bool apb_written;
    bool test_issued;
    bool probe_observed;
};

static struct rp1_irq s_slots[AARCH64_RP1_NUM_IRQS];
static enum rp1_fn_state s_fn_state = RP1_FN_UNCLAIMED;
static uint16_t s_saved_msg_ctrl;
static uint32_t s_saved_vec_ctrl[AARCH64_RP1_NUM_IRQS];
static bool s_vec_saved[AARCH64_RP1_NUM_IRQS];
static struct aarch64_rp1_msi_window s_msi_win;
static bool s_has_msi_win;
static struct rp1_irq *s_uncommitted;

#ifdef OS01_HOST_TEST
void rp1_irq_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_fn_state = RP1_FN_UNCLAIMED;
    s_saved_msg_ctrl = 0;
    memset(s_saved_vec_ctrl, 0, sizeof(s_saved_vec_ctrl));
    memset(s_vec_saved, 0, sizeof(s_vec_saved));
    memset(&s_msi_win, 0, sizeof(s_msi_win));
    s_has_msi_win = false;
    s_uncommitted = NULL;
}
#endif

static inline uint32_t apb_off(uint32_t src)
{
    return RP1_APB_IRQ_BASE + 4U * src;
}

static inline uint32_t tbl_off(uint32_t src, uint32_t field)
{
    return src * 16U + field * 4U;
}

static int pba_read_bit(const struct aarch64_rp1_irq_platform *p, uint32_t src,
                        bool *pend_out)
{
    uint32_t v = 0U;
    int rc = aarch64_rp1_mmio_read32(p, RP1_MMIO_PBA, (src / 32U) * 4U, &v);
    if (rc != 0)
        return rc;
    *pend_out = (v & (1U << (src % 32U))) != 0U;
    return 0;
}

static int table_write_checked(const struct aarch64_rp1_irq_platform *p,
                               uint32_t off, uint32_t val)
{
    uint32_t rb = 0U;
    if (aarch64_rp1_mmio_write32(p, RP1_MMIO_TABLE, off, val) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, off, &rb) != 0 ||
        rb != val)
        return -EIO;
    return 0;
}

static int apb_write_checked(const struct aarch64_rp1_irq_platform *p,
                             uint32_t src, uint32_t val)
{
    uint32_t rb = 0U;
    if (aarch64_rp1_mmio_write32(p, RP1_MMIO_APB, apb_off(src), val) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_APB, apb_off(src), &rb) != 0 ||
        rb != val)
        return -EIO;
    return 0;
}

static int cfg_write16_checked(const struct aarch64_rp1_irq_platform *p,
                               uint16_t off, uint16_t val)
{
    uint16_t rb = 0U;
    if (aarch64_rp1_cfg_write16(p, off, val) != 0 ||
        aarch64_rp1_cfg_read16(p, off, &rb) != 0 ||
        rb != val)
        return -EIO;
    return 0;
}

static bool mip_host_mask_verified(const struct aarch64_rp1_irq_platform *p,
                                   uint32_t hw_slot)
{
    uint32_t off = MIP_INT_MASKL_HOST + (hw_slot >= 32U ? 0x10U : 0U);
    uint32_t bit = 1U << (hw_slot % 32U);
    uint32_t v = 0U;
    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, off, &v) != 0)
        return false;
    if ((v & bit) == 0U) {
        (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_MIP, off, v | bit);
        if (aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, off, &v) != 0)
            return false;
    }
    return (v & bit) != 0U;
}

static void rp1_irq_trampoline(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid;
    (void)regs;
    uint32_t src = (uint32_t)param;
    if (src >= AARCH64_RP1_NUM_IRQS)
        return;
    struct rp1_irq *slot = &s_slots[src];
    if (slot->state != RP1_SLOT_COMMITTED || !slot->fn)
        return;

    slot->fn(slot->arg);

    const struct aarch64_rp1_irq_platform *p = aarch64_rp1_irq_platform_get();
    if (!p)
        return;
    if (slot->trigger == RP1_IRQ_LEVEL_HIGH) {
        (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_APB,
                                       RP1_APB_IRQ_SET_OFF + apb_off(src),
                                       RP1_APB_BIT_IACK);
    }
    (void)aarch64_rp1_irq_flush(p, src);
}

static int fn_takeover_begin(const struct aarch64_rp1_irq_platform *p)
{
    uint16_t ctl = 0U;
    uint16_t ctl_off = (uint16_t)(p->bar0.msix_cap + 2U);
    if (aarch64_rp1_cfg_read16(p, ctl_off, &ctl) != 0)
        return -EIO;
    if ((ctl & RP1_MSIX_CTL_ENABLE) != 0U)
        return -EBUSY; /* Firmware/unknown owner active */

    s_saved_msg_ctrl = ctl;
    uint16_t masked_ctl = (uint16_t)((ctl & ~RP1_MSIX_CTL_ENABLE) | RP1_MSIX_CTL_FMASK);
    if (cfg_write16_checked(p, ctl_off, masked_ctl) != 0)
        return -EIO;

    for (uint32_t i = 0U; i < p->bar0.entries && i < AARCH64_RP1_NUM_IRQS; i++) {
        uint32_t vc = 0U;
        if (aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(i, 3U), &vc) != 0)
            goto fail_restore_masks;
        s_saved_vec_ctrl[i] = vc;
        s_vec_saved[i] = true;
        if (table_write_checked(p, tbl_off(i, 3U), vc | RP1_MSIX_VEC_MASK) != 0)
            goto fail_restore_masks;
    }

    uint16_t enabled_ctl = (uint16_t)((ctl | RP1_MSIX_CTL_ENABLE) & ~RP1_MSIX_CTL_FMASK);
    if (cfg_write16_checked(p, ctl_off, enabled_ctl) != 0)
        goto fail_restore_masks;

    s_fn_state = RP1_FN_PREPARING;
    return 0;

fail_restore_masks:
    for (uint32_t i = 0U; i < p->bar0.entries && i < AARCH64_RP1_NUM_IRQS; i++) {
        if (s_vec_saved[i]) {
            if (table_write_checked(p, tbl_off(i, 3U), s_saved_vec_ctrl[i]) != 0)
                s_fn_state = RP1_FN_QUARANTINED;
            s_vec_saved[i] = false;
        }
    }
    if (cfg_write16_checked(p, ctl_off, s_saved_msg_ctrl) != 0)
        s_fn_state = RP1_FN_QUARANTINED;
    return -EIO;
}

static int fn_takeover_restore(const struct aarch64_rp1_irq_platform *p)
{
    uint16_t ctl_off = (uint16_t)(p->bar0.msix_cap + 2U);
    uint16_t dis_fmask = (uint16_t)((s_saved_msg_ctrl & ~RP1_MSIX_CTL_ENABLE) |
                                    RP1_MSIX_CTL_FMASK);
    if (cfg_write16_checked(p, ctl_off, dis_fmask) != 0)
        return -EIO;
    for (uint32_t i = 0U; i < p->bar0.entries && i < AARCH64_RP1_NUM_IRQS; i++) {
        if (s_vec_saved[i]) {
            if (table_write_checked(p, tbl_off(i, 3U), s_saved_vec_ctrl[i]) != 0)
                return -EIO;
            s_vec_saved[i] = false;
        }
    }
    if (cfg_write16_checked(p, ctl_off, s_saved_msg_ctrl) != 0)
        return -EIO;
    s_fn_state = RP1_FN_UNCLAIMED;
    return 0;
}

static int abort_internal(struct rp1_irq *slot, bool device_mask_verified,
                          const char *stage)
{
    const struct aarch64_rp1_irq_platform *p = aarch64_rp1_irq_platform_get();
    uint32_t src = slot->source;
    bool gic_disabled = false;
    bool rp1_masked = false;
    bool mip_masked = false;
    bool quiescent = false;

    if (p && slot->apb_written) {
        (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_APB,
                                       RP1_APB_IRQ_CLR_OFF + apb_off(src),
                                       RP1_APB_BIT_ENABLE | RP1_APB_BIT_TEST);
    }
    if (p && slot->table_written) {
        (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_TABLE, tbl_off(src, 3U),
                                       slot->saved_tbl_ctrl | RP1_MSIX_VEC_MASK);
    }
    if (slot->mip_route.valid)
        gic_disabled = (bcm2712_mip_disable(&slot->mip_route) == 0);
    if (p)
        (void)aarch64_rp1_irq_flush(p, src);

    if (p && slot->apb_written) {
        uint32_t apb_v = 0U;
        if (aarch64_rp1_mmio_read32(p, RP1_MMIO_APB, apb_off(src), &apb_v) == 0 &&
            (apb_v & (RP1_APB_BIT_ENABLE | RP1_APB_BIT_TEST)) == 0U)
            rp1_masked = true;
    } else if (!slot->apb_written) {
        rp1_masked = true;
    }

    if (p && slot->table_written) {
        uint32_t vc = 0U;
        if (aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(src, 3U), &vc) == 0 &&
            (vc & RP1_MSIX_VEC_MASK) != 0U)
            rp1_masked = true;
    }

    if (slot->mip_route.valid && gic_disabled && rp1_masked && device_mask_verified) {
        bool pba_pend = true;
        if (slot->test_issued && !slot->probe_observed) {
            quiescent = false;
        } else if (p && pba_read_bit(p, src, &pba_pend) == 0 && !pba_pend &&
                   !bcm2712_mip_pending(&slot->mip_route) &&
                   bcm2712_mip_clear_probe(&slot->mip_route) == 0) {
            quiescent = true;
        }
    } else if (!slot->mip_route.valid && !slot->test_issued && device_mask_verified) {
        quiescent = rp1_masked;
    }

    if (quiescent) {
        bool ok = true;
        if (p && slot->apb_written &&
            apb_write_checked(p, src, slot->saved_apb_cfg) != 0)
            ok = false;
        if (ok && p && slot->table_written) {
            if (table_write_checked(p, tbl_off(src, 0U), slot->saved_tbl_lo) != 0 ||
                table_write_checked(p, tbl_off(src, 1U), slot->saved_tbl_hi) != 0 ||
                table_write_checked(p, tbl_off(src, 2U), slot->saved_tbl_data) != 0 ||
                table_write_checked(p, tbl_off(src, 3U), slot->saved_tbl_ctrl) != 0)
                ok = false;
        }
        if (ok && slot->mip_route.valid &&
            bcm2712_mip_release(&slot->mip_route, true) != 0)
            ok = false;

        if (ok && s_fn_state == RP1_FN_PREPARING && p) {
            if (fn_takeover_restore(p) != 0)
                ok = false;
            if (ok && s_has_msi_win) {
                if (aarch64_rp1_msi_window_restore(p, &s_msi_win) != 0)
                    ok = false;
                else
                    s_has_msi_win = false;
            }
        }
        if (ok) {
            s_uncommitted = NULL;
            memset(slot, 0, sizeof(*slot));
            return 0;
        }
    }

    /* Quarantine path */
    if (!gic_disabled && p && slot->mip_route.valid)
        mip_masked = mip_host_mask_verified(p, slot->mip_route.hw_slot);

    if (!gic_disabled && !mip_masked && !rp1_masked) {
        device_fail_unsafe(NULL, "rp1-irq: interrupt containment not verified");
        __builtin_unreachable();
    }

    if (slot->mip_route.valid)
        (void)bcm2712_mip_release(&slot->mip_route, false);
    slot->state = RP1_SLOT_QUARANTINED;
    if (s_fn_state == RP1_FN_PREPARING && p) {
        uint16_t ctl_off = (uint16_t)(p->bar0.msix_cap + 2U);
        uint16_t ctl = 0U;
        if (aarch64_rp1_cfg_read16(p, ctl_off, &ctl) == 0)
            (void)aarch64_rp1_cfg_write16(p, ctl_off, ctl | RP1_MSIX_CTL_FMASK);
        s_fn_state = RP1_FN_QUARANTINED;
    }
    s_uncommitted = NULL;
    log_warn("[rp1-irq-msix] source=%u stage=%s isolated=1\n",
             (unsigned)src, stage ? stage : "abort");
    return -EIO;
}

int rp1_irq_prepare(uint32_t source, enum rp1_irq_trigger trigger,
                    rp1_irq_handler_fn fn, void *arg,
                    struct rp1_irq **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (source >= AARCH64_RP1_NUM_IRQS || !fn)
        return -EINVAL;

    bool is_usb = (source >= RP1_USBHOST_SRC_MIN && source <= RP1_USBHOST_SRC_MAX);
    if ((is_usb && trigger != RP1_IRQ_EDGE_RISING) ||
        (!is_usb && trigger != RP1_IRQ_LEVEL_HIGH))
        return -EINVAL;

    const struct aarch64_rp1_irq_platform *p = aarch64_rp1_irq_platform_get();
    if (!p || !p->valid || source >= p->bar0.entries)
        return -ENODEV;
    if (s_uncommitted != NULL || s_fn_state == RP1_FN_QUARANTINED)
        return -EBUSY;

    struct rp1_irq *slot = &s_slots[source];
    if (slot->state != RP1_SLOT_FREE)
        return -EBUSY;

    uint32_t apb_init = 0U;
    bool pba_init = false;
    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_APB, apb_off(source), &apb_init) != 0 ||
        pba_read_bit(p, source, &pba_init) != 0)
        return -EIO;
    if ((apb_init & (RP1_APB_BIT_ENABLE | RP1_APB_BIT_TEST)) != 0U || pba_init)
        return -EBUSY;

    if (s_fn_state == RP1_FN_UNCLAIMED) {
        int rc = fn_takeover_begin(p);
        if (rc != 0)
            return rc;
    }

    if (!s_has_msi_win) {
        if (aarch64_rp1_msi_window_claim(p, &s_msi_win) != 0) {
            if (s_fn_state == RP1_FN_PREPARING && fn_takeover_restore(p) != 0)
                s_fn_state = RP1_FN_QUARANTINED;
            return -EIO;
        }
        s_has_msi_win = true;
    }

    memset(slot, 0, sizeof(*slot));
    slot->source = source;
    slot->trigger = trigger;
    slot->fn = fn;
    slot->arg = arg;
    slot->state = RP1_SLOT_PREPARING;
    s_uncommitted = slot;

    if (bcm2712_mip_reserve(p, rp1_irq_trampoline, (uint64_t)source, &slot->mip_route) != 0) {
        (void)abort_internal(slot, true, "mip-reserve");
        return -EIO;
    }

    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(source, 0U), &slot->saved_tbl_lo) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(source, 1U), &slot->saved_tbl_hi) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(source, 2U), &slot->saved_tbl_data) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(source, 3U), &slot->saved_tbl_ctrl) != 0) {
        (void)abort_internal(slot, true, "table-read");
        return -EIO;
    }

    slot->table_written = true;
    if (table_write_checked(p, tbl_off(source, 0U), (uint32_t)slot->mip_route.address) != 0 ||
        table_write_checked(p, tbl_off(source, 1U), (uint32_t)(slot->mip_route.address >> 32)) != 0 ||
        table_write_checked(p, tbl_off(source, 2U), slot->mip_route.data) != 0 ||
        table_write_checked(p, tbl_off(source, 3U), slot->saved_tbl_ctrl | RP1_MSIX_VEC_MASK) != 0) {
        (void)abort_internal(slot, true, "table-write");
        return -EIO;
    }

    if (bcm2712_mip_enable(&slot->mip_route) != 0 ||
        table_write_checked(p, tbl_off(source, 3U), slot->saved_tbl_ctrl & ~RP1_MSIX_VEC_MASK) != 0) {
        (void)abort_internal(slot, true, "enable");
        return -EIO;
    }

    slot->saved_apb_cfg = apb_init;
    slot->apb_written = true;
    uint32_t apb_cfg = RP1_APB_BIT_ENABLE |
                       (trigger == RP1_IRQ_LEVEL_HIGH ? RP1_APB_BIT_IACK_EN : 0U);
    if (apb_write_checked(p, source, apb_cfg) != 0) {
        (void)abort_internal(slot, true, "apb-enable");
        return -EIO;
    }

    if (bcm2712_mip_pending(&slot->mip_route)) {
        (void)abort_internal(slot, true, "pre-probe-pending");
        return -EIO;
    }

    /* Real delivery probe (10 ms bounded architected counter) */
    slot->test_issued = true;
    if (aarch64_rp1_mmio_write32(p, RP1_MMIO_APB,
                                 RP1_APB_IRQ_SET_OFF + apb_off(source),
                                 RP1_APB_BIT_TEST) != 0) {
        (void)abort_internal(slot, true, "test-write");
        return -EIO;
    }
    (void)aarch64_rp1_irq_flush(p, source);

    uint64_t freq = rp1_counter_freq();
    uint64_t timeout_ticks = (freq / 1000ULL) * 10ULL;
    if (timeout_ticks == 0ULL)
        timeout_ticks = 10000ULL;
    uint64_t t0 = rp1_counter_now();
    bool observed = false;
    while ((rp1_counter_now() - t0) < timeout_ticks) {
        if (bcm2712_mip_pending(&slot->mip_route)) {
            observed = true;
            break;
        }
    }
    slot->probe_observed = observed;

    (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_APB,
                                 RP1_APB_IRQ_CLR_OFF + apb_off(source),
                                 RP1_APB_BIT_TEST);
    (void)aarch64_rp1_irq_flush(p, source);
    if (trigger == RP1_IRQ_LEVEL_HIGH) {
        (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_APB,
                                     RP1_APB_IRQ_SET_OFF + apb_off(source),
                                     RP1_APB_BIT_IACK);
        (void)aarch64_rp1_irq_flush(p, source);
    }

    if (!observed) {
        log_warn("[rp1-irq-msix] source=%u address=0x%lx data=%u intid=%u probe=timeout\n",
                 (unsigned)source, (unsigned long)slot->mip_route.address,
                 (unsigned)slot->mip_route.data, (unsigned)slot->mip_route.intid);
        (void)abort_internal(slot, true, "probe-timeout");
        return -ETIMEDOUT;
    }

    if (bcm2712_mip_clear_probe(&slot->mip_route) != 0) {
        log_warn("[rp1-irq-msix] source=%u address=0x%lx data=%u intid=%u probe=not-quiet\n",
                 (unsigned)source, (unsigned long)slot->mip_route.address,
                 (unsigned)slot->mip_route.data, (unsigned)slot->mip_route.intid);
        (void)abort_internal(slot, true, "probe-clear");
        return -EIO;
    }

    uint32_t fin_apb = 0U, fin_vc = 0U;
    bool fin_pba = true;
    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_APB, apb_off(source), &fin_apb) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_TABLE, tbl_off(source, 3U), &fin_vc) != 0 ||
        pba_read_bit(p, source, &fin_pba) != 0 ||
        fin_apb != apb_cfg || (fin_vc & RP1_MSIX_VEC_MASK) != 0U ||
        fin_pba || bcm2712_mip_pending(&slot->mip_route)) {
        log_warn("[rp1-irq-msix] source=%u address=0x%lx data=%u intid=%u probe=not-quiet\n",
                 (unsigned)source, (unsigned long)slot->mip_route.address,
                 (unsigned)slot->mip_route.data, (unsigned)slot->mip_route.intid);
        (void)abort_internal(slot, true, "final-verify");
        return -EIO;
    }

    log_info("[rp1-irq-msix] source=%u address=0x%lx data=%u intid=%u probe=ok\n",
             (unsigned)source, (unsigned long)slot->mip_route.address,
             (unsigned)slot->mip_route.data, (unsigned)slot->mip_route.intid);
    *out = slot;
    return 0;
}

int rp1_irq_commit(struct rp1_irq *irq)
{
    if (!irq || irq != s_uncommitted || irq->state != RP1_SLOT_PREPARING)
        return -EINVAL;
    irq->state = RP1_SLOT_COMMITTED;
    s_uncommitted = NULL;
    s_fn_state = RP1_FN_OWNED;
    return 0;
}

int rp1_irq_abort(struct rp1_irq *irq, bool device_mask_verified)
{
    if (!irq || irq != s_uncommitted || irq->state != RP1_SLOT_PREPARING)
        return -EINVAL;
    return abort_internal(irq, device_mask_verified, "caller-abort");
}

uint32_t rp1_irq_intid(const struct rp1_irq *irq)
{
    if (!irq || !irq->mip_route.valid)
        return 0U;
    return irq->mip_route.intid;
}
