/* @os01-test
 * id: test_aarch64_fdt_early
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: aarch64-fdt-early
 * scenarios: [boot.uefi.fdt_early.default]
 */
/* AArch64 loader early-MMIO FDT walker (boot/uefi/arch/aarch64/
 * fdt_early.c) — derives the early console PA and the 2 MiB Device
 * window from the firmware FDT (roadmap P2 hardcode item: head.S
 * Device block slots 0x40..0x4F must come from the platform).
 *
 * The fixture builder emits synthetic device-tree blobs (header +
 * memory reservation + structure + strings blocks) from explicit
 * node/prop lists; expectations are hard values, never derived from
 * the production walker.
 *
 * Cases: the QEMU virt shape (GICv2 with a v2m child + PL011 →
 * console=0x09000000, window=[0x08000000, 0x09200000), 9 blocks),
 * GIC-only, UART-only, no match, far-apart devices past the block
 * cap, 1-cell buses (RPi shape), and one negative per malformed
 * class (bad magic, truncated header, unknown token, old version,
 * oversized offsets, unterminated tree).
 */
#include "page_table_test_runner.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../boot/uefi/arch/aarch64/loader.h"

/* ── Synthetic FDT builder ─────────────────────────────────────── */

#define FW_MAGIC UINT32_C(0xd00dfeed)
#define FDT_BEGIN_NODE_TOKEN UINT32_C(0x00000001)
#define FDT_END_NODE_TOKEN   UINT32_C(0x00000002)
#define FDT_PROP_TOKEN       UINT32_C(0x00000003)
#define FDT_END_TOKEN        UINT32_C(0x00000009)

struct fdt_writer {
    uint8_t buf[4096];
    size_t struct_off;
    size_t struct_len;
    size_t strings_off;
    size_t strings_len;
};

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void fw_init(struct fdt_writer *w, uint32_t version)
{
    memset(w->buf, 0, sizeof(w->buf));
    put_be32(w->buf + 0, FW_MAGIC);
    put_be32(w->buf + 20, version);          /* version */
    put_be32(w->buf + 36, 0);                /* size_struct, fixed at emit */
    w->struct_off = 56;                      /* after header + memrsv */
    w->struct_len = 0;
    w->strings_off = sizeof(w->buf) / 2;
    w->strings_len = 0;
}

static void fw_token(struct fdt_writer *w, uint32_t token)
{
    put_be32(w->buf + w->struct_off + w->struct_len, token);
    w->struct_len += 4;
}

static void fw_begin_node(struct fdt_writer *w, const char *name)
{
    size_t n = strlen(name);

    fw_token(w, FDT_BEGIN_NODE_TOKEN);
    memcpy(w->buf + w->struct_off + w->struct_len, name, n + 1);
    w->struct_len += n + 1;
    w->struct_len = (w->struct_len + 3u) & ~(size_t)3;
}

static size_t fw_strings_add(struct fdt_writer *w, const char *name)
{
    size_t off = w->strings_len;
    size_t n = strlen(name) + 1;

    memcpy(w->buf + w->strings_off + off, name, n);
    w->strings_len += n;
    return off;
}

static void fw_prop(struct fdt_writer *w, const char *name,
                    const void *value, size_t len)
{
    put_be32(w->buf + w->struct_off + w->struct_len, FDT_PROP_TOKEN);
    put_be32(w->buf + w->struct_off + w->struct_len + 4, (uint32_t)len);
    put_be32(w->buf + w->struct_off + w->struct_len + 8,
             (uint32_t)fw_strings_add(w, name));
    w->struct_len += 12;
    memcpy(w->buf + w->struct_off + w->struct_len, value, len);
    w->struct_len += len;
    w->struct_len = (w->struct_len + 3u) & ~(size_t)3;
}

static void fw_end_node(struct fdt_writer *w)
{
    fw_token(w, FDT_END_NODE_TOKEN);
}

static void fw_finish(struct fdt_writer *w, size_t *size_out)
{
    fw_token(w, FDT_END_TOKEN);
    w->strings_off = w->struct_off + w->struct_len;
    memmove(w->buf + w->strings_off, w->buf + sizeof(w->buf) / 2,
            w->strings_len);
    put_be32(w->buf + 4, (uint32_t)(w->strings_off + w->strings_len));
    put_be32(w->buf + 8, (uint32_t)w->struct_off);
    put_be32(w->buf + 12, (uint32_t)w->strings_off);
    put_be32(w->buf + 16, w->struct_off);    /* off_mem_rsv */
    put_be32(w->buf + 32, 0);                /* size_strings (unchecked) */
    put_be32(w->buf + 36, (uint32_t)w->struct_len);
    *size_out = w->strings_off + w->strings_len;
}

/* Prop cell values are BIG-endian on the wire (the walker and every
 * FDT consumer load them that way). */
static void cells64(uint32_t *out, uint64_t v)
{
    put_be32((uint8_t *)&out[0], (uint32_t)(v >> 32));
    put_be32((uint8_t *)&out[1], (uint32_t)v);
}

static void prop_u32(struct fdt_writer *w, const char *name, uint32_t v)
{
    uint8_t be[4];

    put_be32(be, v);
    fw_prop(w, name, be, 4);
}

static void prop_reg_2_2(struct fdt_writer *w, const uint64_t *pairs,
                         size_t count)
{
    uint32_t cells[16];

    for (size_t i = 0; i < count; ++i) {
        cells64(&cells[i * 4 + 0], pairs[i * 2 + 0]);   /* address cells */
        cells64(&cells[i * 4 + 2], pairs[i * 2 + 1]);   /* size cells */
    }
    fw_prop(w, "reg", cells, (uint32_t)(count * 16));
}

/* Node shapes. */
static void fw_root(struct fdt_writer *w, unsigned address_cells,
                    unsigned size_cells)
{
    fw_begin_node(w, "");
    prop_u32(w, "#address-cells", address_cells);
    prop_u32(w, "#size-cells", size_cells);
}

static void fw_uart(struct fdt_writer *w, uint64_t base, uint64_t size)
{
    const char compat[] = "arm,pl011\0arm,primecell";
    const uint64_t reg[2] = { base, size };

    fw_begin_node(w, "uart");
    fw_prop(w, "compatible", compat, sizeof(compat));
    prop_reg_2_2(w, reg, 1);
    fw_end_node(w);
}

static void fw_gic(struct fdt_writer *w, uint64_t gicd, uint64_t gicd_size,
                   uint64_t gicc, uint64_t gicc_size, int with_v2m_child,
                   uint64_t v2m, uint64_t v2m_size)
{
    const char compat[] = "arm,cortex-a15-gic";
    const uint64_t reg[4] = { gicd, gicd_size, gicc, gicc_size };

    fw_begin_node(w, "intc");
    fw_prop(w, "compatible", compat, sizeof(compat));
    prop_reg_2_2(w, reg, 2);
    if (with_v2m_child) {
        const char v2m_compat[] = "arm,gic-v2m-frame";
        const uint64_t v2m_reg[2] = { v2m, v2m_size };

        fw_begin_node(w, "v2m");
        fw_prop(w, "compatible", v2m_compat, sizeof(v2m_compat));
        prop_reg_2_2(w, v2m_reg, 1);
        fw_end_node(w);
    }
    fw_end_node(w);
}

/* ── Cases ─────────────────────────────────────────────────────── */

static void test_fdt_early_qemu_virt_shape(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_gic(&w, 0x08000000, 0x10000, 0x08010000, 0x10000, 1,
           0x08020000, 0x1000);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(1, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
    assert_eq(UINT64_C(0x09000000), out.console_base);
    assert_eq(UINT64_C(0x08000000), out.device_base);
    assert_eq(9u, out.device_blocks);
    assert_eq(0u, out.reserved);
}

static void test_fdt_early_gic_only(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_gic(&w, 0x08000000, 0x1000, 0x08010000, 0x2000, 0, 0, 0);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(1, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
    assert_eq(UINT64_C(0), out.console_base);   /* no UART → fallback */
    assert_eq(UINT64_C(0x08000000), out.device_base);
    assert_eq(1u, out.device_blocks);           /* GICC within block 0x40 */
}

static void test_fdt_early_uart_only(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(1, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
    assert_eq(UINT64_C(0x09000000), out.console_base);
    assert_eq(UINT64_C(0x09000000), out.device_base);
    assert_eq(1u, out.device_blocks);
}

static void test_fdt_early_unaligned_base_snapped_down(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09001000, 0x1000);   /* not 2 MiB-aligned */
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(1, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
    assert_eq(UINT64_C(0x09001000), out.console_base);  /* exact PA */
    assert_eq(UINT64_C(0x09000000), out.device_base);   /* snapped */
    assert_eq(1u, out.device_blocks);
}

static void test_fdt_early_no_match(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;
    const char compat[] = "virtio,mmio";
    const uint64_t reg[2] = { 0x0a000000, 0x200 };

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_begin_node(&w, "virtio");
    fw_prop(&w, "compatible", compat, sizeof(compat));
    prop_reg_2_2(&w, reg, 1);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void test_fdt_early_far_apart_devices_rejected(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    /* RPi4-ish shape: UART and GIC over 1 GiB apart → the contiguous
     * 2 MiB-block window would exceed the 1024-block cap. */
    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_gic(&w, 0x4c800000, 0x1000, 0x4c810000, 0x2000, 0, 0, 0);
    fw_uart(&w, 0xfe201000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void test_fdt_early_one_cell_bus(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;
    const char compat[] = "arm,pl011\0arm,primecell";
    uint8_t reg_cells[8];

    put_be32(&reg_cells[0], 0xfe201000);
    put_be32(&reg_cells[4], 0x1000);

    /* RPi shape: #address-cells/#size-cells = 1/1, UART under /soc
     * which re-specifies its own cell counts. */
    fw_init(&w, 17);
    fw_begin_node(&w, "");
    prop_u32(&w, "#address-cells", 1);
    prop_u32(&w, "#size-cells", 1);
    fw_begin_node(&w, "soc");
    prop_u32(&w, "#address-cells", 1);
    prop_u32(&w, "#size-cells", 1);
    fw_begin_node(&w, "uart");
    fw_prop(&w, "compatible", compat, sizeof(compat));
    fw_prop(&w, "reg", reg_cells, sizeof(reg_cells));
    fw_end_node(&w);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(1, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
    assert_eq(UINT64_C(0xfe201000), out.console_base);
    assert_eq(UINT64_C(0xfe200000), out.device_base);
    assert_eq(1u, out.device_blocks);
}

static void test_fdt_early_bad_magic(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);
    put_be32(w.buf, 0x12345678);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void test_fdt_early_truncated(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, 8, &out));
    assert_eq(0, aarch64_fdt_early_mmio(NULL, 4096, &out));
}

static void test_fdt_early_old_version(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 2);   /* property names pre-v3: unsupported */
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void test_fdt_early_unknown_token(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_token(&w, UINT32_C(0x00000042));   /* undefined token */
    fw_token(&w, FDT_END_TOKEN);
    fw_finish(&w, &size);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void test_fdt_early_offsets_out_of_range(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_finish(&w, &size);

    put_be32(w.buf + 8, UINT32_C(0xffff0000));   /* off_struct past end */
    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void test_fdt_early_unterminated_tree(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;

    /* Structure block ends with END_NODE but never reaches FDT_END:
     * the walker must stop at the block bound, not run off. */
    fw_init(&w, 17);
    fw_root(&w, 2, 2);
    fw_uart(&w, 0x09000000, 0x1000);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(0, aarch64_fdt_early_mmio(w.buf, (uint32_t)size, &out));
}

static void fw_prop_str(struct fdt_writer *w, const char *name, const char *s)
{
    fw_prop(w, name, s, strlen(s) + 1u);
}

static void fw_prop_u32(struct fdt_writer *w, const char *name, uint32_t v)
{
    uint8_t b[4];
    put_be32(b, v);
    fw_prop(w, name, b, sizeof(b));
}

static void fw_prop_cells(struct fdt_writer *w, const char *name,
                          const uint32_t *vals, size_t count)
{
    uint8_t b[64];
    for (size_t i = 0; i < count; i++)
        put_be32(b + i * 4u, vals[i]);
    fw_prop(w, name, b, count * 4u);
}

static void test_fdt_boot_scan_stdout_alias_and_nested_buses(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;
    const uint32_t gic_reg[] = { 0, 0x08000000, 0, 0x10000, 0, 0x08010000, 0, 0x10000 };
    const uint32_t u0_reg[] = { 0, 0x09000000, 0, 0x1000 };
    const uint32_t u1_reg[] = { 0x00020000, 0x1000 };
    const uint32_t bus_ranges[] = { 0x00000000, 0x0, 0x09000000, 0x00100000 };

    fw_init(&w, 17);
    fw_begin_node(&w, "");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_prop_str(&w, "compatible", "linux,dummy-virt");

    fw_begin_node(&w, "chosen");
    fw_prop_str(&w, "stdout-path", "serial1:115200n8");
    fw_end_node(&w);

    fw_begin_node(&w, "aliases");
    fw_prop_str(&w, "serial1", "/bus/sub/pl011@20000");
    fw_end_node(&w);

    fw_begin_node(&w, "intc@8000000");
    fw_prop_str(&w, "compatible", "arm,cortex-a15-gic");
    fw_prop_cells(&w, "reg", gic_reg, 8);
    fw_end_node(&w);

    fw_begin_node(&w, "pl011@9000000");
    fw_prop_str(&w, "compatible", "arm,pl011");
    fw_prop_cells(&w, "reg", u0_reg, 4);
    fw_end_node(&w);

    fw_begin_node(&w, "bus");
    fw_prop_u32(&w, "#address-cells", 1);
    fw_prop_u32(&w, "#size-cells", 1);
    fw_prop_cells(&w, "ranges", bus_ranges, 4);
    fw_begin_node(&w, "sub");
    fw_prop_u32(&w, "#address-cells", 1);
    fw_prop_u32(&w, "#size-cells", 1);
    fw_prop(&w, "ranges", "", 0); /* empty ranges = identity */
    fw_begin_node(&w, "pl011@20000");
    fw_prop_str(&w, "compatible", "arm,pl011");
    fw_prop_cells(&w, "reg", u1_reg, 2);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_end_node(&w);

    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(AARCH64_FDT_SCAN_OK,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size, "linux,dummy-virt",
                                    0, &out));
    assert_eq(UINT64_C(0x09020000), out.console_base);
    assert_eq(UINT64_C(0x08000000), out.device_base);
    assert_eq(9u, out.device_blocks);

    assert_eq(AARCH64_FDT_SCAN_PLATFORM_MISMATCH,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size, "raspberrypi,5-model-b",
                                    0, &out));
}

static void test_fdt_boot_scan_rejects_bad_stdout_and_missing_ranges(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;
    const uint32_t gic_reg[] = { 0, 0x08000000, 0, 0x10000, 0, 0x08010000, 0, 0x10000 };
    const uint32_t u0_reg[] = { 0, 0x09000000, 0, 0x1000 };

    /* Disabled stdout-path target */
    fw_init(&w, 17);
    fw_begin_node(&w, "");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_prop_str(&w, "compatible", "linux,dummy-virt");
    fw_begin_node(&w, "chosen");
    fw_prop_str(&w, "stdout-path", "/pl011@9000000");
    fw_end_node(&w);
    fw_begin_node(&w, "intc@8000000");
    fw_prop_str(&w, "compatible", "arm,cortex-a15-gic");
    fw_prop_cells(&w, "reg", gic_reg, 8);
    fw_end_node(&w);
    fw_begin_node(&w, "pl011@9000000");
    fw_prop_str(&w, "compatible", "arm,pl011");
    fw_prop_str(&w, "status", "disabled");
    fw_prop_cells(&w, "reg", u0_reg, 4);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);
    assert_eq(AARCH64_FDT_SCAN_MISSING_MMIO,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size, NULL, 0, &out));

    /* Non-existent stdout-path target */
    fw_init(&w, 17);
    fw_begin_node(&w, "");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_begin_node(&w, "chosen");
    fw_prop_str(&w, "stdout-path", "/no-such-uart");
    fw_end_node(&w);
    fw_begin_node(&w, "intc@8000000");
    fw_prop_str(&w, "compatible", "arm,cortex-a15-gic");
    fw_prop_cells(&w, "reg", gic_reg, 8);
    fw_end_node(&w);
    fw_begin_node(&w, "pl011@9000000");
    fw_prop_str(&w, "compatible", "arm,pl011");
    fw_prop_cells(&w, "reg", u0_reg, 4);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);
    assert_eq(AARCH64_FDT_SCAN_MISSING_MMIO,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size, NULL, 0, &out));

    /* Intermediate bus missing ranges property */
    fw_init(&w, 17);
    fw_begin_node(&w, "");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_begin_node(&w, "intc@8000000");
    fw_prop_str(&w, "compatible", "arm,cortex-a15-gic");
    fw_prop_cells(&w, "reg", gic_reg, 8);
    fw_end_node(&w);
    fw_begin_node(&w, "bus");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_begin_node(&w, "pl011@9000000");
    fw_prop_str(&w, "compatible", "arm,pl011");
    fw_prop_cells(&w, "reg", u0_reg, 4);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);
    assert_eq(AARCH64_FDT_SCAN_MISSING_MMIO,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size, NULL, 0, &out));
}

static void test_fdt_boot_scan_rpi5_firmware_mode_and_partial_ranges(void)
{
    struct fdt_writer w;
    struct aarch64_early_mmio_handoff out;
    size_t size;
    const uint32_t soc_ranges[] = {
        0x7fff9000, 0x10, 0x7fff9000, 0x00003000
    };
    const uint32_t gic_ok[] = {
        0x7fff9000, 0x1000,
        0x7fffa000, 0x2000
    };
    const uint32_t gic_cross_end[] = {
        0x7fff9000, 0x1000,
        0x7fffa000, 0x3000 /* ends at 0x7fffd000, past 0x7fffc000 */
    };

    /* Pi GIC-only fixture in firmware console mode */
    fw_init(&w, 17);
    fw_begin_node(&w, "");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_prop_str(&w, "compatible", "raspberrypi,5-model-b");
    fw_begin_node(&w, "soc");
    fw_prop_u32(&w, "#address-cells", 1);
    fw_prop_u32(&w, "#size-cells", 1);
    fw_prop_cells(&w, "ranges", soc_ranges, 4);
    fw_begin_node(&w, "interrupt-controller@7fff9000");
    fw_prop_str(&w, "compatible", "arm,gic-400");
    fw_prop_cells(&w, "reg", gic_ok, 4);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(AARCH64_FDT_SCAN_OK,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size,
                                    "raspberrypi,5-model-b",
                                    UINT64_C(0x1f00030000), &out));
    assert_eq(UINT64_C(0x1f00030000), out.console_base);
    assert_eq(UINT64_C(0x107fe00000), out.device_base);
    assert_eq(1u, out.device_blocks);

    /* GICC crosses bus mapping endpoint -> rejected */
    fw_init(&w, 17);
    fw_begin_node(&w, "");
    fw_prop_u32(&w, "#address-cells", 2);
    fw_prop_u32(&w, "#size-cells", 2);
    fw_prop_str(&w, "compatible", "raspberrypi,5-model-b");
    fw_begin_node(&w, "soc");
    fw_prop_u32(&w, "#address-cells", 1);
    fw_prop_u32(&w, "#size-cells", 1);
    fw_prop_cells(&w, "ranges", soc_ranges, 4);
    fw_begin_node(&w, "interrupt-controller@7fff9000");
    fw_prop_str(&w, "compatible", "arm,gic-400");
    fw_prop_cells(&w, "reg", gic_cross_end, 4);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_end_node(&w);
    fw_finish(&w, &size);

    assert_eq(AARCH64_FDT_SCAN_MISSING_MMIO,
              aarch64_fdt_boot_scan(w.buf, (uint32_t)size,
                                    "raspberrypi,5-model-b",
                                    UINT64_C(0x1f00030000), &out));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fdt_early_qemu_virt_shape),
    TEST_ENTRY(test_fdt_early_gic_only),
    TEST_ENTRY(test_fdt_early_uart_only),
    TEST_ENTRY(test_fdt_early_unaligned_base_snapped_down),
    TEST_ENTRY(test_fdt_early_no_match),
    TEST_ENTRY(test_fdt_early_far_apart_devices_rejected),
    TEST_ENTRY(test_fdt_early_one_cell_bus),
    TEST_ENTRY(test_fdt_early_bad_magic),
    TEST_ENTRY(test_fdt_early_truncated),
    TEST_ENTRY(test_fdt_early_old_version),
    TEST_ENTRY(test_fdt_early_unknown_token),
    TEST_ENTRY(test_fdt_early_offsets_out_of_range),
    TEST_ENTRY(test_fdt_early_unterminated_tree),
    TEST_ENTRY(test_fdt_boot_scan_stdout_alias_and_nested_buses),
    TEST_ENTRY(test_fdt_boot_scan_rejects_bad_stdout_and_missing_ranges),
    TEST_ENTRY(test_fdt_boot_scan_rpi5_firmware_mode_and_partial_ranges),
TEST_LIST_END

int main(void)
{
    return PAGE_TABLE_RUN_ALL_TESTS();
}
