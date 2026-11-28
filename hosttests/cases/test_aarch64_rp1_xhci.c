/* @os01-test
 * id: test_aarch64_rp1_xhci
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: pr
 * link: rp1-xhci
 * scenarios: [drivers.usb.aarch64.rp1]
 */
#include "test_framework.h"
#include <arch/aarch64/rp1_xhci.h>
#include <arch/aarch64/rp1_pcie.h>
#include <driver/xhci.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Minimal FDT v17 builder for deterministic unit fixtures ─────────── */
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_END        9U

struct fdt_buf {
    uint8_t struct_buf[4096];
    uint32_t struct_len;
    char str_buf[1024];
    uint32_t str_len;
    uint8_t blob[8192];
    uint32_t blob_len;
};

static void fdt_be32_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int aarch64_rp1_mmio_read32(const struct aarch64_rp1_irq_platform *p,
                            enum aarch64_rp1_mmio_space space,
                            uint32_t off, uint32_t *value)
{
    (void)p; (void)space; (void)off; (void)value;
    return -ENOSYS;
}

int aarch64_rp1_mmio_write32(const struct aarch64_rp1_irq_platform *p,
                             enum aarch64_rp1_mmio_space space,
                             uint32_t off, uint32_t value)
{
    (void)p; (void)space; (void)off; (void)value;
    return -ENOSYS;
}

static void fdt_init(struct fdt_buf *f) { memset(f, 0, sizeof(*f)); }

static void fdt_u32(struct fdt_buf *f, uint32_t v)
{
    fdt_be32_put(f->struct_buf + f->struct_len, v);
    f->struct_len += 4U;
}

static uint32_t fdt_add_str(struct fdt_buf *f, const char *s)
{
    uint32_t off = 0U;
    while (off < f->str_len) {
        if (strcmp(f->str_buf + off, s) == 0)
            return off;
        off += (uint32_t)strlen(f->str_buf + off) + 1U;
    }
    uint32_t len = (uint32_t)strlen(s) + 1U;
    memcpy(f->str_buf + f->str_len, s, len);
    uint32_t ret = f->str_len;
    f->str_len += len;
    return ret;
}

static void fdt_begin(struct fdt_buf *f, const char *name)
{
    fdt_u32(f, FDT_BEGIN_NODE);
    uint32_t len = (uint32_t)strlen(name) + 1U;
    memcpy(f->struct_buf + f->struct_len, name, len);
    f->struct_len += len;
    while (f->struct_len & 3U)
        f->struct_buf[f->struct_len++] = 0U;
}

static void fdt_end(struct fdt_buf *f) { fdt_u32(f, FDT_END_NODE); }

static void fdt_prop(struct fdt_buf *f, const char *name, const void *val, uint32_t len)
{
    uint32_t soff = fdt_add_str(f, name);
    fdt_u32(f, FDT_PROP);
    fdt_u32(f, len);
    fdt_u32(f, soff);
    if (len > 0U)
        memcpy(f->struct_buf + f->struct_len, val, len);
    f->struct_len += len;
    while (f->struct_len & 3U)
        f->struct_buf[f->struct_len++] = 0U;
}

static void fdt_prop_u32(struct fdt_buf *f, const char *name, uint32_t v)
{
    uint8_t b[4];
    fdt_be32_put(b, v);
    fdt_prop(f, name, b, 4U);
}

static void fdt_prop_u32s(struct fdt_buf *f, const char *name, const uint32_t *vals, size_t n)
{
    uint8_t b[64];
    for (size_t i = 0; i < n; i++)
        fdt_be32_put(b + i * 4U, vals[i]);
    fdt_prop(f, name, b, (uint32_t)(n * 4U));
}

static uint32_t fdt_finish(struct fdt_buf *f)
{
    fdt_u32(f, FDT_END);
    uint32_t hdr_sz = 40U;
    uint32_t rsv_off = hdr_sz;
    uint32_t struct_off = rsv_off + 16U;
    uint32_t str_off = struct_off + f->struct_len;
    uint32_t total = str_off + f->str_len;

    memset(f->blob, 0, total);
    fdt_be32_put(f->blob + 0, 0xd00dfeedU);
    fdt_be32_put(f->blob + 4, total);
    fdt_be32_put(f->blob + 8, struct_off);
    fdt_be32_put(f->blob + 12, str_off);
    fdt_be32_put(f->blob + 16, rsv_off);
    fdt_be32_put(f->blob + 20, 17U);
    fdt_be32_put(f->blob + 24, 16U);
    fdt_be32_put(f->blob + 28, 0U);
    fdt_be32_put(f->blob + 32, f->str_len);
    fdt_be32_put(f->blob + 36, f->struct_len);
    memcpy(f->blob + struct_off, f->struct_buf, f->struct_len);
    memcpy(f->blob + str_off, f->str_buf, f->str_len);
    f->blob_len = total;
    return total;
}

static void build_valid_rpi5_dtb(struct fdt_buf *f, bool include_disabled, bool bad_pci_cell, bool dup_range, bool wrong_rc)
{
    fdt_init(f);
    fdt_begin(f, "");
    fdt_prop_u32(f, "#address-cells", 2U);
    fdt_prop_u32(f, "#size-cells", 2U);
    fdt_prop(f, "compatible", "raspberrypi,5-model-b\0brcm,bcm2712", 35U);

    fdt_begin(f, "axi");
    fdt_prop_u32(f, "#address-cells", 2U);
    fdt_prop_u32(f, "#size-cells", 1U);
    const uint32_t axi_ranges[] = { 0x00U, 0x00U, 0x10U, 0x00U, 0x04000000U };
    fdt_prop_u32s(f, "ranges", axi_ranges, 5U);

    fdt_begin(f, "pcie@120000");
    fdt_prop(f, "compatible", wrong_rc ? "pci-host-ecam-generic" : "brcm,bcm2712-pcie",
             wrong_rc ? 22U : 18U);
    fdt_prop(f, "status", "okay", 5U);
    fdt_prop_u32(f, "#address-cells", 3U);
    fdt_prop_u32(f, "#size-cells", 2U);
    const uint32_t rc_reg[] = { 0x00U, 0x00120000U, 0x00009310U };
    fdt_prop_u32s(f, "reg", rc_reg, 3U);

    fdt_begin(f, "rp1@0,0");
    fdt_prop_u32(f, "#address-cells", 2U);
    fdt_prop_u32(f, "#size-cells", 2U);
    /* child: (0x00, 0x00000000) <- parent PCI: (0x02000000, 0x1f, 0x00000000), size (0x0, 0x00400000) */
    const uint32_t rp1_ranges[] = {
        0x00U, 0x00000000U,
        bad_pci_cell ? 0x01000000U : 0x02000000U, 0x1fU, 0x00000000U,
        0x00U, 0x00400000U,
    };
    fdt_prop_u32s(f, "ranges", rp1_ranges, 7U);

    fdt_begin(f, "usb@200000");
    fdt_prop(f, "compatible", "snps,dwc3", 10U);
    fdt_prop(f, "status", "okay", 5U);
    fdt_prop(f, "dr_mode", "host", 5U);
    const uint32_t u0_reg[] = { 0x00U, 0x00200000U, 0x00U, 0x00100000U };
    fdt_prop_u32s(f, "reg", u0_reg, 4U);
    fdt_end(f);

    fdt_begin(f, "usb@300000");
    fdt_prop(f, "compatible", "snps,dwc3", 10U);
    fdt_prop(f, "status", "okay", 5U);
    fdt_prop(f, "dr_mode", "host", 5U);
    const uint32_t u1_reg[] = { 0x00U, dup_range ? 0x00200000U : 0x00300000U, 0x00U, 0x00100000U };
    fdt_prop_u32s(f, "reg", u1_reg, 4U);
    fdt_end(f);

    if (include_disabled) {
        fdt_begin(f, "usb@400000");
        fdt_prop(f, "compatible", "snps,dwc3", 10U);
        fdt_prop(f, "status", "disabled", 9U);
        fdt_prop(f, "dr_mode", "host", 5U);
        const uint32_t u2_reg[] = { 0x00U, 0x00400000U, 0x00U, 0x00100000U };
        fdt_prop_u32s(f, "reg", u2_reg, 4U);
        fdt_end(f);
    }

    fdt_end(f); /* rp1 */
    fdt_end(f); /* pcie */
    fdt_end(f); /* axi */
    fdt_end(f); /* root */
    fdt_finish(f);
}

/* ── Fake BCM2712 RC MMIO for WoR resource decoding ──────────────────── */
struct fake_rc {
    uint32_t status;         /* 0x4068 */
    uint32_t bus_num;        /* 0x0018 */
    uint32_t mem_base_lim;   /* 0x0020 */
    uint32_t out_pci_lo;     /* 0x400c */
    uint32_t out_pci_hi;     /* 0x4010 */
    uint32_t out_cpu_bl;     /* 0x4070 */
    uint32_t out_cpu_bhi;    /* 0x4080 */
    uint32_t out_cpu_lhi;    /* 0x4084 */
    uint32_t bar2_lo;        /* 0x4034 */
    uint32_t bar2_hi;        /* 0x4038 */
    uint32_t ubus_remap_lo;  /* 0x40ac */
    uint32_t ubus_remap_hi;  /* 0x40b0 */
    uint32_t ep_id;          /* 0x8000 */
    uint32_t ep_cmd;         /* 0x8004 */
    uint32_t ep_bar1_lo;     /* 0x8014 */
    uint32_t ep_bar1_hi;     /* 0x8018 */
    uint32_t selector;
    uint32_t selector_calls;
    uint32_t restore_calls;
};

static uint32_t fake_rc_read32(void *ctx, uint32_t off)
{
    struct fake_rc *rc = (struct fake_rc *)ctx;
    switch (off) {
    case 0x0018U: return rc->bus_num;
    case 0x0020U: return rc->mem_base_lim;
    case 0x400cU: return rc->out_pci_lo;
    case 0x4010U: return rc->out_pci_hi;
    case 0x4034U: return rc->bar2_lo;
    case 0x4038U: return rc->bar2_hi;
    case 0x4068U: return rc->status;
    case 0x4070U: return rc->out_cpu_bl;
    case 0x4080U: return rc->out_cpu_bhi;
    case 0x4084U: return rc->out_cpu_lhi;
    case 0x40acU: return rc->ubus_remap_lo;
    case 0x40b0U: return rc->ubus_remap_hi;
    case 0x8000U: return rc->ep_id;
    case 0x8004U: return rc->ep_cmd;
    case 0x8014U: return rc->ep_bar1_lo;
    case 0x8018U: return rc->ep_bar1_hi;
    default: return 0U;
    }
}

static uint32_t fake_cfg_save_select(void *ctx, uint32_t bdf)
{
    struct fake_rc *rc = (struct fake_rc *)ctx;
    uint32_t prev = rc->selector;
    rc->selector = bdf;
    rc->selector_calls++;
    return prev;
}

static void fake_cfg_restore(void *ctx, uint32_t saved)
{
    struct fake_rc *rc = (struct fake_rc *)ctx;
    rc->selector = saved;
    rc->restore_calls++;
}

static void init_valid_wor_rc(struct fake_rc *rc)
{
    memset(rc, 0, sizeof(*rc));
    rc->status = 0x30U; /* PHYLINKUP | DL_ACTIVE */
    rc->bus_num = 0x00010100U; /* primary=0, secondary=1, sub=1 */
    /* Bridge mem window: PCI 0x00000000 .. 0x0fffffff */
    rc->mem_base_lim = 0x0ff00000U;
    /* Outbound win0: PCI 0x00000000 -> CPU [0xb0000000, 0xc0000000) */
    rc->out_pci_lo = 0x00000000U;
    rc->out_pci_hi = 0x00000000U;
    rc->out_cpu_bl = 0xbff0b000U;
    rc->out_cpu_bhi = 0x00000000U;
    rc->out_cpu_lhi = 0x00000000U;
    /* Inbound BAR2: identity [0, 4 GiB) (size_enc = 0x11 -> 1<<(17+15) = 4 GiB) */
    rc->bar2_lo = 0x00000011U;
    rc->bar2_hi = 0x00000000U;
    rc->ubus_remap_lo = 0x00000001U; /* ACCESS_EN=1, base=0 */
    rc->ubus_remap_hi = 0x00000000U;
    /* RP1 endpoint: VID=0x1de4, DID=0x0001, MEM+BME, BAR1=0x00000000 (32-bit) */
    rc->ep_id = 0x00011de4U;
    rc->ep_cmd = 0x0006U;
    rc->ep_bar1_lo = 0x04000000U; /* PCI 0x04000000 -> CPU 0xb4000000 */
    rc->selector = 0xDEADBEEFU;
}

static void test_dtb_parse_valid_and_disabled(void)
{
    struct fdt_buf f;
    build_valid_rpi5_dtb(&f, true, false, false, false);
    struct aarch64_rp1_xhci_desc desc;
    assert_eq(0, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &desc));
    assert_true(desc.valid);
    assert_eq(2U, desc.controller_count);
    assert_eq(0x1000120000ULL, desc.rc_reg_base);
    assert_eq(0x9310ULL, desc.rc_reg_size);
    assert_eq(0x200000ULL, desc.controllers[0].rp1_offset);
    assert_eq(0x100000ULL, desc.controllers[0].size);
    assert_eq(0x300000ULL, desc.controllers[1].rp1_offset);
    assert_eq(0x100000ULL, desc.controllers[1].size);
}

static void test_dtb_parse_rejects_bad_pci_cell_dup_and_wrong_rc(void)
{
    struct fdt_buf f;
    struct aarch64_rp1_xhci_desc desc;

    build_valid_rpi5_dtb(&f, false, true, false, false);
    assert_eq(-EINVAL, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &desc));
    assert_false(desc.valid);

    build_valid_rpi5_dtb(&f, false, false, true, false);
    assert_eq(-EINVAL, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &desc));
    assert_false(desc.valid);

    build_valid_rpi5_dtb(&f, false, false, false, true);
    assert_eq(-ENODEV, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &desc));
    assert_false(desc.valid);
}

static void test_rp1_pcie_resources_wor_decode_and_exclusions(void)
{
    struct fdt_buf f;
    build_valid_rpi5_dtb(&f, false, false, false, false);
    struct aarch64_rp1_xhci_desc desc;
    assert_eq(0, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &desc));

    struct fake_rc rc;
    init_valid_wor_rc(&rc);
    struct aarch64_rp1_pcie_io io = {
        .ctx = &rc,
        .rc_read32 = fake_rc_read32,
        .cfg_save_select = fake_cfg_save_select,
        .cfg_restore = fake_cfg_restore,
    };
    struct aarch64_rp1_pcie_resources res;
    assert_eq(0, aarch64_rp1_pcie_resources(&desc, &io, &res));
    assert_true(res.valid);
    assert_eq(0xb4000000ULL, res.bar1_cpu_base);
    assert_eq(0ULL, res.dma_min_pa);
    assert_eq(0xb0000000ULL, res.dma_limit_pa);
    assert_eq(1U, rc.selector_calls);
    assert_eq(1U, rc.restore_calls);
    assert_eq(0xDEADBEEFU, rc.selector);
}

static void test_rp1_pcie_resources_link_down_and_selector_restore_on_fail(void)
{
    struct fdt_buf f;
    build_valid_rpi5_dtb(&f, false, false, false, false);
    struct aarch64_rp1_xhci_desc desc;
    assert_eq(0, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &desc));

    struct fake_rc rc;
    init_valid_wor_rc(&rc);
    rc.status = 0x0U; /* link down */
    struct aarch64_rp1_pcie_io io = {
        .ctx = &rc,
        .rc_read32 = fake_rc_read32,
        .cfg_save_select = fake_cfg_save_select,
        .cfg_restore = fake_cfg_restore,
    };
    struct aarch64_rp1_pcie_resources res;
    assert_true(aarch64_rp1_pcie_resources(&desc, &io, &res) != 0);
    assert_eq(0U, rc.selector_calls);
    assert_eq(0U, rc.restore_calls);

    /* Link up, bad BAR1 / disabled BME -> must restore selector */
    init_valid_wor_rc(&rc);
    rc.ep_cmd = 0x0002U; /* BME off */
    assert_true(aarch64_rp1_pcie_resources(&desc, &io, &res) != 0);
    assert_eq(1U, rc.selector_calls);
    assert_eq(1U, rc.restore_calls);
    assert_eq(0xDEADBEEFU, rc.selector);
}

int main(void)
{
    TEST_SUITE("aarch64_rp1_xhci");
    test_dtb_parse_valid_and_disabled();
    test_dtb_parse_rejects_bad_pci_cell_dup_and_wrong_rc();
    test_rp1_pcie_resources_wor_decode_and_exclusions();
    test_rp1_pcie_resources_link_down_and_selector_restore_on_fail();
    TEST_RESULTS();
    return __test_stats.failed ? 1 : 0;
}
