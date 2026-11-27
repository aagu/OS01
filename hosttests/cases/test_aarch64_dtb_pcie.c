/* @os01-test
 * id: test_aarch64_dtb_pcie
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: pr
 * link: aarch64-dtb-pcie
 * scenarios: [memory.aarch64.dtb_pcie]
 */
#include "test_framework.h"
#include <arch/aarch64/pcie.h>
#include <arch/pci.h>
#include <log/log.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

int g_log_level = LOG_WARN;
static int s_warn_count = 0;

void _log_info_impl(const char *fmt, ...)
{
    (void)fmt;
}

void _log_warn_impl(const char *fmt, ...)
{
    (void)fmt;
    s_warn_count++;
}

/* ── Minimal FDT v17 test builder ──────────────────────────────────── */
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_END        9U

struct fdt_buf {
    uint8_t st[4096];
    uint32_t st_len;
    uint8_t str[512];
    uint32_t str_len;
    uint8_t blob[8192];
    uint32_t blob_len;
};

static void fdt_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void fb_init(struct fdt_buf *f)
{
    memset(f, 0, sizeof(*f));
}

static void fb_st_u32(struct fdt_buf *f, uint32_t v)
{
    fdt_put_be32(f->st + f->st_len, v);
    f->st_len += 4;
}

static void fb_begin_node(struct fdt_buf *f, const char *name)
{
    fb_st_u32(f, FDT_BEGIN_NODE);
    size_t len = strlen(name) + 1;
    memcpy(f->st + f->st_len, name, len);
    f->st_len += (uint32_t)len;
    while (f->st_len & 3U) {
        f->st[f->st_len++] = 0;
    }
}

static void fb_end_node(struct fdt_buf *f)
{
    fb_st_u32(f, FDT_END_NODE);
}

static uint32_t fb_intern_str(struct fdt_buf *f, const char *s)
{
    uint32_t off = 0;
    while (off < f->str_len) {
        if (strcmp((const char *)(f->str + off), s) == 0) {
            return off;
        }
        off += (uint32_t)strlen((const char *)(f->str + off)) + 1;
    }
    uint32_t res = f->str_len;
    size_t len = strlen(s) + 1;
    memcpy(f->str + f->str_len, s, len);
    f->str_len += (uint32_t)len;
    return res;
}

static void fb_prop_raw(struct fdt_buf *f, const char *name, const void *data, uint32_t len)
{
    uint32_t nameoff = fb_intern_str(f, name);
    fb_st_u32(f, FDT_PROP);
    fb_st_u32(f, len);
    fb_st_u32(f, nameoff);
    if (len > 0) {
        memcpy(f->st + f->st_len, data, len);
        f->st_len += len;
        while (f->st_len & 3U) {
            f->st[f->st_len++] = 0;
        }
    }
}

static void fb_prop_cells(struct fdt_buf *f, const char *name, const uint32_t *cells, uint32_t count)
{
    uint8_t tmp[256];
    for (uint32_t i = 0; i < count; i++) {
        fdt_put_be32(tmp + i * 4U, cells[i]);
    }
    fb_prop_raw(f, name, tmp, count * 4U);
}

static void fb_prop_u32(struct fdt_buf *f, const char *name, uint32_t val)
{
    fb_prop_cells(f, name, &val, 1);
}

static void fb_prop_str(struct fdt_buf *f, const char *name, const char *s)
{
    fb_prop_raw(f, name, s, (uint32_t)strlen(s) + 1U);
}

static void fb_finish(struct fdt_buf *f)
{
    fb_st_u32(f, FDT_END);
    uint32_t rsv_off = 40;
    uint32_t rsv_size = 16; /* one empty terminator entry */
    uint32_t st_off = rsv_off + rsv_size;
    uint32_t str_off = st_off + f->st_len;
    uint32_t total = str_off + f->str_len;

    memset(f->blob, 0, total);
    fdt_put_be32(f->blob + 0, 0xd00dfeedU);
    fdt_put_be32(f->blob + 4, total);
    fdt_put_be32(f->blob + 8, st_off);
    fdt_put_be32(f->blob + 12, str_off);
    fdt_put_be32(f->blob + 16, rsv_off);
    fdt_put_be32(f->blob + 20, 17); /* version */
    fdt_put_be32(f->blob + 24, 16); /* last_comp_version */
    fdt_put_be32(f->blob + 32, f->str_len);
    fdt_put_be32(f->blob + 36, f->st_len);

    memcpy(f->blob + st_off, f->st, f->st_len);
    memcpy(f->blob + str_off, f->str, f->str_len);
    f->blob_len = total;
}

TEST_FUNC(test_qemu_virt_ecam_dtb)
{
    struct fdt_buf f;
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);

    fb_begin_node(&f, "intc@8000000");
    fb_prop_u32(&f, "phandle", 0x8002U);
    fb_prop_raw(&f, "interrupt-controller", NULL, 0);
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#interrupt-cells", 3);
    fb_end_node(&f);

    fb_begin_node(&f, "pcie@10000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_str(&f, "device_type", "pci");
    fb_prop_u32(&f, "#address-cells", 3);
    fb_prop_u32(&f, "#size-cells", 2);
    fb_prop_u32(&f, "#interrupt-cells", 1);
    fb_prop_u32(&f, "linux,pci-domain", 0);
    {
        uint32_t mask[] = { 0x1800U, 0x0U, 0x0U, 0x7U };
        fb_prop_cells(&f, "interrupt-map-mask", mask, 4);
    }
    {
        /* Each entry: 4 child cells + 1 phandle + 2 parent addr + 3 parent intr = 10 cells */
        uint32_t imap[] = {
            0x0000U, 0U, 0U, 1U, 0x8002U, 0U, 0U, 0U, 3U, 4U,
            0x0000U, 0U, 0U, 2U, 0x8002U, 0U, 0U, 0U, 4U, 4U,
            0x0800U, 0U, 0U, 1U, 0x8002U, 0U, 0U, 0U, 4U, 4U,
            0x0800U, 0U, 0U, 2U, 0x8002U, 0U, 0U, 0U, 5U, 4U,
        };
        fb_prop_cells(&f, "interrupt-map", imap, 40);
    }
    {
        uint32_t br[] = { 0x00, 0xff };
        fb_prop_cells(&f, "bus-range", br, 2);
    }
    {
        uint32_t reg[] = { 0x40, 0x10000000, 0x00, 0x10000000 };
        fb_prop_cells(&f, "reg", reg, 4);
    }
    {
        /* 3-cell pci + 2-cell parent + 2-cell size = 7 cells per entry */
        uint32_t ranges[] = {
            /* IO: 0x0 -> 0x3eff0000, size 0x10000 */
            0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x3eff0000U, 0x00000000U, 0x00010000U,
            /* MMIO32: 0x10000000 -> 0x10000000, size 0x2eff0000 */
            0x02000000U, 0x00000000U, 0x10000000U, 0x00000000U, 0x10000000U, 0x00000000U, 0x2eff0000U,
            /* MMIO64 (prefetch): 0x80_00000000 -> 0x80_00000000, size 0x80_00000000 */
            0x43000000U, 0x00000080U, 0x00000000U, 0x00000080U, 0x00000000U, 0x00000080U, 0x00000000U,
        };
        fb_prop_cells(&f, "ranges", ranges, 21);
    }
    fb_end_node(&f);
    fb_end_node(&f);
    fb_finish(&f);

    struct aarch64_pcie_topology topo;
    s_warn_count = 0;
    assert_eq(0, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_eq(0, s_warn_count);
    assert_eq(1, topo.root_count);

    const struct aarch64_pcie_root_desc *r = &topo.roots[0];
    assert_true(r->valid);
    assert_eq(AARCH64_PCIE_CTRL_ECAM_GENERIC, r->kind);
    assert_eq(0, r->domain);
    assert_eq(0, r->bus_start);
    assert_eq(255, r->bus_end);
    assert_eq(0x4010000000ULL, r->reg_cpu_base);
    assert_eq(0x10000000ULL, r->reg_size);
    assert_eq(3, r->range_count);

    assert_eq(PCI_SPACE_IO, r->ranges[0].space);
    assert_false(r->ranges[0].prefetchable);
    assert_eq(0x0ULL, r->ranges[0].pci_base);
    assert_eq(0x3eff0000ULL, r->ranges[0].cpu_base);
    assert_eq(0x10000ULL, r->ranges[0].size);

    assert_eq(PCI_SPACE_MMIO32, r->ranges[1].space);
    assert_false(r->ranges[1].prefetchable);
    assert_eq(0x10000000ULL, r->ranges[1].pci_base);
    assert_eq(0x10000000ULL, r->ranges[1].cpu_base);
    assert_eq(0x2eff0000ULL, r->ranges[1].size);

    assert_eq(PCI_SPACE_MMIO64, r->ranges[2].space);
    assert_true(r->ranges[2].prefetchable);
    assert_eq(0x8000000000ULL, r->ranges[2].pci_base);
    assert_eq(0x8000000000ULL, r->ranges[2].cpu_base);
    assert_eq(0x8000000000ULL, r->ranges[2].size);

    assert_true(r->has_int_map);
    assert_eq(4, r->int_map_count);
    assert_eq(0x1800U, r->int_map_mask[0]);
    assert_eq(0x7U, r->int_map_mask[3]);
    assert_eq(0x0000U, r->int_map[0].addr_hi);
    assert_eq(1U, r->int_map[0].int_pin);
    assert_eq(35U, r->int_map[0].gic_intid);
    assert_eq(0x0800U, r->int_map[2].addr_hi);
    assert_eq(1U, r->int_map[2].int_pin);
    assert_eq(36U, r->int_map[2].gic_intid);
    assert_eq(0x0800U, r->int_map[3].addr_hi);
    assert_eq(2U, r->int_map[3].int_pin);
    assert_eq(37U, r->int_map[3].gic_intid);
}

TEST_FUNC(test_interrupt_map_parent_zero_ac_and_bad_phandle)
{
    /* 1) intc without #address-cells defaults parent #address-cells to 0 */
    struct fdt_buf f;
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);

    fb_begin_node(&f, "pcie@10000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_u32(&f, "#address-cells", 3);
    fb_prop_u32(&f, "#size-cells", 2);
    fb_prop_u32(&f, "#interrupt-cells", 1);
    {
        uint32_t reg[] = { 0x0, 0x10000000, 0x0, 0x00200000 };
        fb_prop_cells(&f, "reg", reg, 4);
    }
    {
        uint32_t imap[] = {
            0x0800U, 0U, 0U, 1U, 0x9001U, 0U, 10U, 4U,
        };
        fb_prop_cells(&f, "interrupt-map", imap, 8);
    }
    fb_end_node(&f);

    /* intc placed AFTER pcie node, with no #address-cells */
    fb_begin_node(&f, "intc@8000000");
    fb_prop_u32(&f, "phandle", 0x9001U);
    fb_prop_raw(&f, "interrupt-controller", NULL, 0);
    fb_prop_u32(&f, "#interrupt-cells", 3);
    fb_end_node(&f);

    fb_end_node(&f);
    fb_finish(&f);

    struct aarch64_pcie_topology topo;
    s_warn_count = 0;
    assert_eq(0, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_eq(0, s_warn_count);
    assert_eq(1, topo.root_count);
    assert_true(topo.roots[0].has_int_map);
    assert_eq(1, topo.roots[0].int_map_count);
    assert_eq(42U, topo.roots[0].int_map[0].gic_intid);

    /* 2) Unknown phandle disables int_map gracefully without dropping ECAM root */
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);
    fb_begin_node(&f, "pcie@10000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_u32(&f, "#address-cells", 3);
    fb_prop_u32(&f, "#size-cells", 2);
    {
        uint32_t reg[] = { 0x0, 0x10000000, 0x0, 0x00200000 };
        fb_prop_cells(&f, "reg", reg, 4);
    }
    {
        uint32_t imap[] = { 0x0U, 0U, 0U, 1U, 0xDEADU, 0U, 3U, 4U };
        fb_prop_cells(&f, "interrupt-map", imap, 8);
    }
    fb_end_node(&f);
    fb_end_node(&f);
    fb_finish(&f);

    s_warn_count = 0;
    assert_eq(0, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_true(s_warn_count > 0);
    assert_eq(1, topo.root_count);
    assert_false(topo.roots[0].has_int_map);
    assert_eq(0, topo.roots[0].int_map_count);
}

TEST_FUNC(test_rpi5_and_disabled_ecam_skipped)
{
    struct fdt_buf f;
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);

    fb_begin_node(&f, "axi");
    fb_prop_str(&f, "compatible", "simple-bus");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);

    fb_begin_node(&f, "pcie@100000");
    fb_prop_str(&f, "compatible", "brcm,bcm2712-pcie");
    fb_prop_str(&f, "status", "disabled");
    fb_end_node(&f);

    fb_begin_node(&f, "pcie@120000");
    fb_prop_str(&f, "compatible", "brcm,bcm2712-pcie");
    fb_prop_str(&f, "status", "okay");
    {
        uint32_t reg[] = { 0x10, 0x120000, 0x0, 0x9310 };
        fb_prop_cells(&f, "reg", reg, 4);
    }
    fb_end_node(&f);

    fb_begin_node(&f, "pcie@20000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_str(&f, "status", "disabled");
    fb_end_node(&f);

    fb_end_node(&f); /* axi */
    fb_end_node(&f); /* root */
    fb_finish(&f);

    struct aarch64_pcie_topology topo;
    s_warn_count = 0;
    assert_eq(0, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_eq(0, s_warn_count);
    assert_eq(0, topo.root_count);
}

TEST_FUNC(test_simple_bus_ancestor_translation)
{
    struct fdt_buf f;
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);

    /* simple-bus maps child 0x00_00000000 -> CPU 0x10_00000000 (size 0x20000000) */
    fb_begin_node(&f, "soc");
    fb_prop_str(&f, "compatible", "simple-bus");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);
    {
        uint32_t sranges[] = { 0x0, 0x00000000, 0x10, 0x00000000, 0x0, 0x20000000 };
        fb_prop_cells(&f, "ranges", sranges, 6);
    }

    fb_begin_node(&f, "pcie@1000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_u32(&f, "#address-cells", 3);
    fb_prop_u32(&f, "#size-cells", 2);
    fb_prop_str(&f, "status", "ok");
    {
        uint32_t reg[] = { 0x0, 0x01000000, 0x0, 0x00400000 }; /* 4 MiB = 4 buses */
        fb_prop_cells(&f, "reg", reg, 4);
    }
    {
        uint32_t ranges[] = {
            /* MMIO32: pci 0x20000000 -> bus parent 0x02000000 -> CPU 0x10_02000000 */
            0x02000000U, 0x0U, 0x20000000U, 0x0U, 0x02000000U, 0x0U, 0x01000000U,
        };
        fb_prop_cells(&f, "ranges", ranges, 7);
    }
    fb_end_node(&f);
    fb_end_node(&f);
    fb_end_node(&f);
    fb_finish(&f);

    struct aarch64_pcie_topology topo;
    s_warn_count = 0;
    assert_eq(0, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_eq(0, s_warn_count);
    assert_eq(1, topo.root_count);

    const struct aarch64_pcie_root_desc *r = &topo.roots[0];
    assert_eq(0x1001000000ULL, r->reg_cpu_base);
    assert_eq(0x00400000ULL, r->reg_size);
    assert_eq(0, r->bus_start);
    assert_eq(3, r->bus_end);
    assert_eq(1, r->range_count);
    assert_eq(0x20000000ULL, r->ranges[0].pci_base);
    assert_eq(0x1002000000ULL, r->ranges[0].cpu_base);
    assert_eq(0x01000000ULL, r->ranges[0].size);
}

TEST_FUNC(test_bad_node_isolation_and_fdt_errors)
{
    /* 1) Bad node alongside a valid node -> rc==0, bad node skipped, valid kept */
    struct fdt_buf f;
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_prop_u32(&f, "#address-cells", 2);
    fb_prop_u32(&f, "#size-cells", 2);

    fb_begin_node(&f, "pcie@10000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_u32(&f, "#address-cells", 3);
    fb_prop_u32(&f, "#size-cells", 2);
    {
        uint32_t br[] = { 10, 5 }; /* invalid: start > end */
        fb_prop_cells(&f, "bus-range", br, 2);
    }
    {
        uint32_t reg[] = { 0x0, 0x10000000, 0x0, 0x1000000 };
        fb_prop_cells(&f, "reg", reg, 4);
    }
    fb_end_node(&f);

    fb_begin_node(&f, "pcie@20000000");
    fb_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fb_prop_u32(&f, "#address-cells", 3);
    fb_prop_u32(&f, "#size-cells", 2);
    {
        uint32_t br[] = { 0, 3 };
        fb_prop_cells(&f, "bus-range", br, 2);
    }
    {
        uint32_t reg[] = { 0x0, 0x20000000, 0x0, 0x400000 };
        fb_prop_cells(&f, "reg", reg, 4);
    }
    fb_end_node(&f);
    fb_end_node(&f);
    fb_finish(&f);

    struct aarch64_pcie_topology topo;
    s_warn_count = 0;
    assert_eq(0, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_true(s_warn_count >= 1);
    assert_eq(1, topo.root_count);
    assert_eq(0x20000000ULL, topo.roots[0].reg_cpu_base);

    /* 2) Unterminated compatible string -> structural error (-1) and zeroed out */
    fb_init(&f);
    fb_begin_node(&f, "");
    fb_begin_node(&f, "pcie@10000000");
    fb_prop_raw(&f, "compatible", "pci-host-ecam-generic", 21); /* missing NUL */
    fb_end_node(&f);
    fb_end_node(&f);
    fb_finish(&f);

    topo.root_count = 99;
    assert_eq(-1, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_eq(0, topo.root_count);

    /* 3) Bad magic / truncated size -> -1 and zeroed out */
    f.blob[0] = 0xAA;
    topo.root_count = 77;
    assert_eq(-1, aarch64_dtb_parse_pcie(f.blob, f.blob_len, &topo));
    assert_eq(0, topo.root_count);

    assert_eq(-1, aarch64_dtb_parse_pcie(f.blob, 20, &topo));
    assert_eq(-1, aarch64_dtb_parse_pcie(NULL, 100, &topo));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_qemu_virt_ecam_dtb),
    TEST_ENTRY(test_interrupt_map_parent_zero_ac_and_bad_phandle),
    TEST_ENTRY(test_rpi5_and_disabled_ecam_skipped),
    TEST_ENTRY(test_simple_bus_ancestor_translation),
    TEST_ENTRY(test_bad_node_isolation_and_fdt_errors),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
