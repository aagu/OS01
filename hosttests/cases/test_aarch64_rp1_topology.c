/* @os01-test
 * id: test_aarch64_rp1_topology
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: nightly
 * link: rp1-topology
 * scenarios: [drivers.irq.aarch64.rp1_topology]
 */
#include "test_framework.h"
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/rp1_xhci.h>
#include <arch/aarch64/boot_mmio.h>
#include <core/bootinfo.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Minimal FDT v17 builder ──────────────────────────────────────────── */
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_END        9U

struct fdt_buf {
    uint8_t struct_buf[8192];
    uint32_t struct_len;
    char str_buf[1024];
    uint32_t str_len;
    uint8_t blob[16384];
    uint32_t blob_len;
};

static void fdt_be32_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
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

static void fdt_prop_u64(struct fdt_buf *f, const char *name, uint64_t v)
{
    uint8_t b[8];
    fdt_be32_put(b, (uint32_t)(v >> 32));
    fdt_be32_put(b + 4, (uint32_t)v);
    fdt_prop(f, name, b, 8U);
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

#define PH_GIC 1U
#define PH_MIP 2U
#define PH_RP1 3U

struct dtb_opts {
    bool reorder_props;
    bool wrong_mip_gic_phandle;
    bool inherit_uart_parent;
    bool ambiguous_rc;
    bool offset_oob;
    bool disabled_uart;
    bool include_xhci;
    bool bad_mip_status;
};

static void build_rpi5_irq_dtb(struct fdt_buf *f, const struct dtb_opts *o)
{
    fdt_init(f);
    fdt_begin(f, "");
    fdt_prop_u32(f, "#address-cells", 2U);
    fdt_prop_u32(f, "#size-cells", 2U);
    fdt_prop(f, "compatible", "raspberrypi,5-model-b\0brcm,bcm2712", 35U);
    fdt_prop_u32(f, "interrupt-parent", PH_GIC);

    /* GICv2 at 0x10_7fff9000 */
    fdt_begin(f, "soc");
    fdt_prop_u32(f, "#address-cells", 2U);
    fdt_prop_u32(f, "#size-cells", 2U);
    fdt_begin(f, "interrupt-controller@107fff9000");
    fdt_prop(f, "compatible", "arm,gic-400", 12U);
    fdt_prop_u32(f, "phandle", PH_GIC);
    const uint32_t gic_reg[] = {
        0x10U, 0x7fff9000U, 0x0U, 0x2000U,
        0x10U, 0x7fffa000U, 0x0U, 0x2000U,
    };
    fdt_prop_u32s(f, "reg", gic_reg, 8U);
    fdt_end(f);
    fdt_end(f);

    /* AXI bus: 0x00000000 -> 0x10_00000000 */
    fdt_begin(f, "axi");
    const uint32_t axi_ranges[] = { 0x00U, 0x00U, 0x10U, 0x00U, 0x04000000U };
    if (o->reorder_props) {
        fdt_prop_u32s(f, "ranges", axi_ranges, 5U);
        fdt_prop_u32(f, "#size-cells", 1U);
        fdt_prop_u32(f, "#address-cells", 2U);
    } else {
        fdt_prop_u32(f, "#address-cells", 2U);
        fdt_prop_u32(f, "#size-cells", 1U);
        fdt_prop_u32s(f, "ranges", axi_ranges, 5U);
    }

    /* MIP @ 0x130000 -> 0x10_00130000 */
    fdt_begin(f, "msi-controller@130000");
    fdt_prop(f, "compatible", "brcm,bcm2712-mip-intc", 22U);
    if (o->bad_mip_status)
        fdt_prop(f, "status", "disabled", 9U);
    else
        fdt_prop(f, "status", "okay", 5U);
    fdt_prop_u32(f, "phandle", PH_MIP);
    if (o->wrong_mip_gic_phandle)
        fdt_prop_u32(f, "interrupt-parent", 999U);
    const uint32_t mip_reg[] = { 0x00U, 0x00130000U, 0x000000c0U };
    fdt_prop_u32s(f, "reg", mip_reg, 3U);
    fdt_prop_u64(f, "brcm,msi-pci-addr", UINT64_C(0xfffffff000));
    fdt_prop_u32(f, "brcm,msi-base-spi", 128U);
    fdt_prop_u32(f, "brcm,msi-num-spis", 64U);
    fdt_prop_u32(f, "brcm,msi-offset", o->offset_oob ? 4U : 0U);
    fdt_end(f);

    if (o->ambiguous_rc) {
        fdt_begin(f, "pcie@110000");
        fdt_prop(f, "compatible", "brcm,bcm2712-pcie", 18U);
        fdt_prop(f, "status", "okay", 5U);
        fdt_prop_u32(f, "#address-cells", 3U);
        fdt_prop_u32(f, "#size-cells", 2U);
        fdt_prop_u32(f, "msi-parent", PH_MIP);
        const uint32_t rc0_reg[] = { 0x00U, 0x00110000U, 0x00009310U };
        fdt_prop_u32s(f, "reg", rc0_reg, 3U);
        fdt_begin(f, "rp1@0,0");
        fdt_prop_u32(f, "#address-cells", 2U);
        fdt_prop_u32(f, "#size-cells", 2U);
        const uint32_t r0[] = { 0x00U, 0x00U, 0x02000000U, 0x1eU, 0x00U, 0x00U, 0x00400000U };
        fdt_prop_u32s(f, "ranges", r0, 7U);
        fdt_end(f);
        fdt_end(f);
    }

    /* RC pcie@120000 -> 0x10_00120000 */
    fdt_begin(f, "pcie@120000");
    fdt_prop(f, "compatible", "brcm,bcm2712-pcie", 18U);
    fdt_prop(f, "status", "okay", 5U);
    fdt_prop_u32(f, "msi-parent", PH_MIP);
    const uint32_t pcie_ranges[] = {
        0x02000000U, 0x00U, 0x00000000U,
        0x1fU, 0x00000000U,
        0x00U, 0xfffffffcU,
    };
    fdt_prop_u32s(f, "ranges", pcie_ranges, 7U);
    const uint32_t rc_reg[] = { 0x00U, 0x00120000U, 0x00009310U };
    if (o->reorder_props) {
        fdt_prop_u32s(f, "reg", rc_reg, 3U);
        fdt_prop_u32(f, "#size-cells", 2U);
        fdt_prop_u32(f, "#address-cells", 3U);
    } else {
        fdt_prop_u32(f, "#address-cells", 3U);
        fdt_prop_u32(f, "#size-cells", 2U);
        fdt_prop_u32s(f, "reg", rc_reg, 3U);
    }

    fdt_begin(f, "rp1@0,0");
    fdt_prop_u32(f, "phandle", PH_RP1);
    if (o->inherit_uart_parent)
        fdt_prop_u32(f, "interrupt-parent", PH_MIP);
    const uint32_t rp1_ranges[] = {
        0xc0U, 0x40000000U,
        0x02000000U, 0x00U, 0x00000000U,
        0x00U, 0x00400000U,
    };
    if (o->reorder_props) {
        fdt_prop_u32s(f, "ranges", rp1_ranges, 7U);
        fdt_prop_u32(f, "#size-cells", 2U);
        fdt_prop_u32(f, "#address-cells", 2U);
    } else {
        fdt_prop_u32(f, "#address-cells", 2U);
        fdt_prop_u32(f, "#size-cells", 2U);
        fdt_prop_u32s(f, "ranges", rp1_ranges, 7U);
    }

    /* RP1 UART0 @ offset 0x30000, source 25, level-high (4), real RPi5 compatible */
    fdt_begin(f, "serial@30000");
    const uint32_t uart_intr[] = { 25U, 4U };
    const uint32_t uart_reg[] = { 0xc0U, 0x40030000U, 0x00U, 0x00000100U };
    if (o->reorder_props) {
        fdt_prop_u32s(f, "interrupts", uart_intr, 2U);
        fdt_prop_u32s(f, "reg", uart_reg, 4U);
        if (!o->inherit_uart_parent)
            fdt_prop_u32(f, "interrupt-parent", PH_RP1);
        fdt_prop(f, "compatible", "arm,pl011-axi\0arm,primecell", 28U);
        fdt_prop(f, "status", o->disabled_uart ? "disabled" : "okay",
                 o->disabled_uart ? 9U : 5U);
    } else {
        fdt_prop(f, "compatible", "arm,pl011", 10U);
        fdt_prop(f, "status", o->disabled_uart ? "disabled" : "okay",
                 o->disabled_uart ? 9U : 5U);
        if (!o->inherit_uart_parent)
            fdt_prop_u32(f, "interrupt-parent", PH_RP1);
        fdt_prop_u32s(f, "reg", uart_reg, 4U);
        fdt_prop_u32s(f, "interrupts", uart_intr, 2U);
    }
    fdt_end(f);

    if (o->include_xhci) {
        fdt_begin(f, "usb@200000");
        fdt_prop(f, "compatible", "snps,dwc3", 10U);
        fdt_prop(f, "status", "okay", 5U);
        fdt_prop(f, "dr_mode", "host", 5U);
        const uint32_t u0_reg[] = { 0xc0U, 0x40200000U, 0x00U, 0x00100000U };
        fdt_prop_u32s(f, "reg", u0_reg, 4U);
        fdt_end(f);
    }

    fdt_end(f); /* rp1 */
    fdt_end(f); /* pcie */
    fdt_end(f); /* axi */
    fdt_end(f); /* root */
    fdt_finish(f);
}

/* ── Stubs for boot_mmio, logger, and descriptors ─────────────────────── */
static struct aarch64_rp1_irq_desc g_stub_irq_desc;
const struct aarch64_rp1_irq_desc *aarch64_rp1_irq_desc_get(void)
{
    return &g_stub_irq_desc;
}

int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

struct mapped_span {
    uint64_t base;
    uint64_t size;
};
static struct mapped_span g_mapped[16];
static unsigned g_mapped_count;
static int g_mmio_init_rc;
static uint64_t g_reject_ram_pa;
static uint64_t g_reject_ram_size;

int aarch64_boot_mmio_ctx_init(const struct boot_context *boot,
                               struct aarch64_boot_mmio_ctx *out)
{
    (void)boot;
    g_mapped_count = 0;
    if (g_mmio_init_rc != 0)
        return g_mmio_init_rc;
    memset(out, 0, sizeof(*out));
    return 0;
}

int aarch64_boot_mmio_map(const struct aarch64_boot_mmio_ctx *ctx,
                          uint64_t base, uint64_t size)
{
    (void)ctx;
    if (g_reject_ram_size != 0 &&
        base < g_reject_ram_pa + g_reject_ram_size &&
        base + size > g_reject_ram_pa)
        return -EFAULT;
    if (g_mapped_count < 16u) {
        g_mapped[g_mapped_count].base = base;
        g_mapped[g_mapped_count].size = size;
        g_mapped_count++;
    }
    return 0;
}

/* ── Fake PCIe RC + RP1 MMIO ──────────────────────────────────────────── */
struct fake_hw {
    uint32_t rc[0x10000 / 4];
    uint32_t ep_cfg[256 / 4];
    uint32_t mip[0x1000 / 4];
    uint32_t apb[0x1000 / 4];
    uint32_t msix_table[64 * 4];
    uint32_t msix_pba[4];
    uint32_t cur_sel;
    unsigned sel_writes;
    unsigned dma_reg_reads;
    unsigned table_pba_accesses;
    bool inject_sel_dirty;
};

static struct fake_hw g_hw;
static uint64_t g_test_rc_pa;
static uint64_t g_test_mip_pa;
static uint64_t g_test_bar0_pa;
static uint64_t g_test_bar1_pa;

static uint32_t fake_rc_read32(void *ctx, uint32_t off)
{
    struct fake_hw *h = (struct fake_hw *)ctx;
    if (off == 0x4034U || off == 0x4038U || off == 0x40acU || off == 0x40b0U)
        h->dma_reg_reads++;
    if (off >= 0x8000U && off < 0x8100U)
        return h->ep_cfg[(off - 0x8000U) / 4U];
    if (off < sizeof(h->rc))
        return h->rc[off / 4U];
    return 0xffffffffU;
}

static uint32_t fake_cfg_save_select(void *ctx, uint32_t bdf)
{
    struct fake_hw *h = (struct fake_hw *)ctx;
    uint32_t prev = h->cur_sel;
    h->cur_sel = bdf;
    h->sel_writes++;
    return prev;
}

static void fake_cfg_restore(void *ctx, uint32_t saved)
{
    struct fake_hw *h = (struct fake_hw *)ctx;
    h->cur_sel = saved;
    h->sel_writes++;
}

static void init_fake_hw(void)
{
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.cur_sel = 0x12340000U;
    g_hw.rc[0x4068U / 4U] = 0x30U;          /* link up */
    g_hw.rc[0x0018U / 4U] = 0x00010100U;    /* sec_bus = 1 */
    g_hw.rc[0x0020U / 4U] = 0x01f00000U;    /* bridge [0x00000000..0x01ffffff] */
    g_hw.rc[0x400cU / 4U] = 0x00000000U;    /* out_pci_lo = 0 */
    g_hw.rc[0x4010U / 4U] = 0x00000000U;    /* out_pci_hi = 0 */
    g_hw.rc[0x4070U / 4U] = 0x01f00000U;    /* out_cpu_bl [0..0x01ffffff] */
    g_hw.rc[0x4080U / 4U] = 0x0000001fU;    /* out_cpu_bhi = 0x1f */
    g_hw.rc[0x4084U / 4U] = 0x0000001fU;    /* out_cpu_lhi = 0x1f */
    /* Inbound BAR2 invalid by default? No, valid unless test breaks it */
    g_hw.rc[0x4034U / 4U] = 16U;            /* 2 GiB */
    g_hw.rc[0x40acU / 4U] = 1U;             /* remap enable */

    /* Endpoint 1de4:0001 rev 2, MEM+BME, 32-bit BAR0=0x00100000 at 0x10, 32-bit BAR1=0x00400000 at 0x14 */
    g_hw.ep_cfg[0x00U / 4U] = 0x00011de4U;
    g_hw.ep_cfg[0x04U / 4U] = 0x00100006U;
    g_hw.ep_cfg[0x08U / 4U] = 0x02000002U;
    g_hw.ep_cfg[0x10U / 4U] = 0x00100000U; /* 32-bit BAR0 */
    g_hw.ep_cfg[0x14U / 4U] = 0x00400000U; /* 32-bit BAR1 */
    g_hw.ep_cfg[0x18U / 4U] = 0x00800000U; /* BAR2 (must NOT be read as BAR1!) */
    g_hw.ep_cfg[0x1cU / 4U] = 0x00000000U;
    g_hw.ep_cfg[0x34U / 4U] = 0x50U;
    g_hw.ep_cfg[0x50U / 4U] = (60U << 16) | 0x0011U; /* MSI-X, 61 entries */
    g_hw.ep_cfg[0x54U / 4U] = 0x00000000U;
    g_hw.ep_cfg[0x58U / 4U] = 0x00002000U;
    memcpy(&g_hw.rc[0x8000U / 4U], g_hw.ep_cfg, sizeof(g_hw.ep_cfg));
}

/* Hosttest MMIO translator override used by rp1_pcie_hw.c under OS01_HOST_TEST */
void *aarch64_rp1_test_pa_to_va(uint64_t pa)
{
    if (pa >= g_test_rc_pa && pa < g_test_rc_pa + 0x10000ULL)
        return (uint8_t *)g_hw.rc + (pa - g_test_rc_pa);
    if (pa >= g_test_mip_pa && pa < g_test_mip_pa + 0x1000ULL)
        return (uint8_t *)g_hw.mip + (pa - g_test_mip_pa);
    if (pa >= g_test_bar1_pa + 0x108000ULL && pa < g_test_bar1_pa + 0x109000ULL)
        return (uint8_t *)g_hw.apb + (pa - (g_test_bar1_pa + 0x108000ULL));
    if (pa >= g_test_bar0_pa && pa < g_test_bar0_pa + 0x2000ULL) {
        g_hw.table_pba_accesses++;
        return (uint8_t *)g_hw.msix_table + (pa - g_test_bar0_pa);
    }
    if (pa >= g_test_bar0_pa + 0x2000ULL && pa < g_test_bar0_pa + 0x4000ULL) {
        g_hw.table_pba_accesses++;
        return (uint8_t *)g_hw.msix_pba + (pa - (g_test_bar0_pa + 0x2000ULL));
    }
    return NULL;
}

static void init_valid_ctx(struct boot_context *ctx)
{
    boot_context_init(ctx);
    ctx->version = BOOT_CONTEXT_VERSION_RP1_PCI;
    ctx->size = sizeof(*ctx);
    ctx->flags = BOOT_CONTEXT_HAS_MEMORY_MAP | BOOT_CONTEXT_HAS_RP1_PCI;
    ctx->rp1_pci.bar0_pci = UINT64_C(0x100000);
    ctx->rp1_pci.bar0_cpu = UINT64_C(0x1f00100000);
    ctx->rp1_pci.bar0_size = UINT64_C(0x4000);
    ctx->rp1_pci.table_offset = 0U;
    ctx->rp1_pci.pba_offset = 0x2000U;
    ctx->rp1_pci.segment = 0U;
    ctx->rp1_pci.entries = 61U;
    ctx->rp1_pci.bus = 1U;
    ctx->rp1_pci.device = 0U;
    ctx->rp1_pci.function = 0U;
    ctx->rp1_pci.msix_cap = 0x50U;
}

static void test_dtb_irq_topology_and_edges(void)
{
    static struct fdt_buf f;
    struct aarch64_rp1_irq_desc d1, d2;
    struct aarch64_rp1_xhci_desc xdesc;
    struct dtb_opts opts = { .include_xhci = false };

    /* 1. Valid DTB without xHCI */
    build_rpi5_irq_dtb(&f, &opts);
    assert_eq(0, aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                           UINT64_C(0x107fff9000),
                                           UINT64_C(0x1f00030000), &d1));
    assert_true(d1.valid);
    assert_true(d1.uart_valid);
    assert_eq(25u, d1.uart_source);
    assert_eq(UINT64_C(0x30000), d1.uart_offset);
    assert_eq(UINT64_C(0x1000130000), d1.mip_cpu_pa);
    assert_eq(UINT64_C(0xfffffff000), d1.msi_pci_addr);
    assert_eq(128u, d1.base_spi);
    assert_eq(64u, d1.count);
    assert_eq(0u, d1.offset);

    /* 2. Reordered properties yield identical result */
    opts.reorder_props = true;
    build_rpi5_irq_dtb(&f, &opts);
    assert_eq(0, aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                           UINT64_C(0x107fff9000),
                                           UINT64_C(0x1f00030000), &d2));
    assert_eq(0, memcmp(&d1, &d2, sizeof(d1)));

    /* 3. Disabled UART with exact PA match is valid */
    opts = (struct dtb_opts){ .disabled_uart = true };
    build_rpi5_irq_dtb(&f, &opts);
    assert_eq(0, aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                           UINT64_C(0x107fff9000),
                                           UINT64_C(0x1f00030000), &d2));
    assert_true(d2.valid && d2.uart_valid);

    /* 4. Wrong console_pa -> valid=true, uart_valid=false */
    assert_eq(0, aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                           UINT64_C(0x107fff9000),
                                           UINT64_C(0x1f00040000), &d2));
    assert_true(d2.valid && !d2.uart_valid);

    /* 5. Inherited UART parent (from rp1 -> mip) -> valid=true, uart_valid=false */
    opts = (struct dtb_opts){ .inherit_uart_parent = true };
    build_rpi5_irq_dtb(&f, &opts);
    assert_eq(0, aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                           UINT64_C(0x107fff9000),
                                           UINT64_C(0x1f00030000), &d2));
    assert_true(d2.valid && !d2.uart_valid);

    /* 6. Wrong MIP/GIC phandle -> valid=false */
    opts = (struct dtb_opts){ .wrong_mip_gic_phandle = true };
    build_rpi5_irq_dtb(&f, &opts);
    assert_true(aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                          UINT64_C(0x107fff9000),
                                          UINT64_C(0x1f00030000), &d2) < 0);
    assert_false(d2.valid);

    /* 7. Ambiguous RC -> valid=false */
    opts = (struct dtb_opts){ .ambiguous_rc = true };
    build_rpi5_irq_dtb(&f, &opts);
    assert_true(aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                          UINT64_C(0x107fff9000),
                                          UINT64_C(0x1f00030000), &d2) < 0);
    assert_false(d2.valid);

    /* 8. Offset OOB -> valid=false */
    opts = (struct dtb_opts){ .offset_oob = true };
    build_rpi5_irq_dtb(&f, &opts);
    assert_true(aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                          UINT64_C(0x107fff9000),
                                          UINT64_C(0x1f00030000), &d2) < 0);
    assert_false(d2.valid);

    /* 9. Optional IRQ failure does NOT affect xHCI */
    opts = (struct dtb_opts){ .bad_mip_status = true, .include_xhci = true };
    build_rpi5_irq_dtb(&f, &opts);
    assert_true(aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                          UINT64_C(0x107fff9000),
                                          UINT64_C(0x1f00030000), &d2) < 0);
    assert_eq(0, aarch64_dtb_parse_rp1_xhci(f.blob, f.blob_len, &xdesc));
    assert_true(xdesc.valid && xdesc.controller_count == 1u);
}

static void test_endpoint_and_prepare_boundaries(void)
{
    static struct fdt_buf f;
    struct dtb_opts opts = { .include_xhci = false };
    struct aarch64_rp1_endpoint ep;
    struct boot_context ctx;
    struct aarch64_rp1_pcie_io io = {
        .ctx = &g_hw,
        .rc_read32 = fake_rc_read32,
        .cfg_save_select = fake_cfg_save_select,
        .cfg_restore = fake_cfg_restore,
    };

    build_rpi5_irq_dtb(&f, &opts);
    assert_eq(0, aarch64_dtb_parse_rp1_irq(f.blob, f.blob_len,
                                           UINT64_C(0x107fff9000),
                                           UINT64_C(0x1f00030000),
                                           &g_stub_irq_desc));

    init_fake_hw();
    g_hw.rc[0x4034U / 4U] = 0U; /* break xHCI inbound DMA: endpoint must still succeed! */
    assert_eq(0, aarch64_rp1_pcie_endpoint(&g_stub_irq_desc, &io, &ep));
    assert_eq(0u, g_hw.dma_reg_reads);
    assert_eq(0x12340000u, g_hw.cur_sel);
    assert_eq(2u, g_hw.sel_writes);
    assert_eq(1u, ep.bus);
    assert_eq(2u, ep.revision);

    /* Real RPi 5 hardware reports RP1 Revision ID 0x00 */
    g_hw.ep_cfg[0x08U / 4U] = 0x02000000U;
    assert_eq(0, aarch64_rp1_pcie_endpoint(&g_stub_irq_desc, &io, &ep));
    assert_eq(0u, ep.revision);
    g_hw.ep_cfg[0x08U / 4U] = 0x02000002U;
    assert_eq(UINT64_C(0x100000), ep.bar0_pci);
    assert_eq(UINT64_C(0x1f00100000), ep.bar0_cpu);
    assert_eq(UINT64_C(0x400000), ep.bar1_pci);
    assert_eq(UINT64_C(0x1f00400000), ep.bar1_cpu);

    /* Prepare success & bounded MMIO/CFG access */
    g_test_rc_pa = g_stub_irq_desc.rc_reg_base;
    g_test_mip_pa = g_stub_irq_desc.mip_cpu_pa;
    g_test_bar0_pa = UINT64_C(0x1f00100000);
    g_test_bar1_pa = UINT64_C(0x1f00400000);
    g_mmio_init_rc = 0;
    g_reject_ram_size = 0;
    init_valid_ctx(&ctx);

    aarch64_rp1_pcie_prepare(&ctx);
    const struct aarch64_rp1_irq_platform *p = aarch64_rp1_irq_platform_get();
    assert_true(p != NULL);
    assert_true(g_mapped_count >= 4u);

    uint32_t val32 = 0;
    uint16_t val16 = 0;
    assert_eq(0, aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, 0x20U, &val32));
    assert_true(aarch64_rp1_mmio_write32(p, RP1_MMIO_PBA, 0x0U, 0U) < 0);
    assert_true(aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, 0x22U, &val32) < 0);
    assert_true(aarch64_rp1_mmio_read32(p, RP1_MMIO_APB, 0x1000U, &val32) < 0);
    assert_eq(0, aarch64_rp1_cfg_read16(p, 0x52U, &val16));
    assert_true(aarch64_rp1_cfg_read16(p, 0x51U, &val16) < 0);
    assert_eq(0, aarch64_rp1_irq_flush(p, 25U));
    assert_true(aarch64_rp1_irq_flush(p, 61U) < 0);

    /* Handoff mismatch must NOT access table/PBA */
    g_hw.table_pba_accesses = 0;
    init_valid_ctx(&ctx);
    ctx.rp1_pci.bus = 2u; /* BDF mismatch */
    aarch64_rp1_pcie_prepare(&ctx);
    assert_true(aarch64_rp1_irq_platform_get() == NULL);
    assert_eq(0u, g_hw.table_pba_accesses);

    init_valid_ctx(&ctx);
    ctx.rp1_pci.entries = 60u; /* entries mismatch */
    aarch64_rp1_pcie_prepare(&ctx);
    assert_true(aarch64_rp1_irq_platform_get() == NULL);
    assert_eq(0u, g_hw.table_pba_accesses);

    /* RAM overlap rejected */
    init_valid_ctx(&ctx);
    g_reject_ram_pa = UINT64_C(0x1f00100000);
    g_reject_ram_size = UINT64_C(0x200000);
    aarch64_rp1_pcie_prepare(&ctx);
    assert_true(aarch64_rp1_irq_platform_get() == NULL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_dtb_irq_topology_and_edges),
    TEST_ENTRY(test_endpoint_and_prepare_boundaries),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 RP1 topology and MMIO");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
