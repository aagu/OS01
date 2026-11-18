/* boot/uefi/arch/aarch64/fdt_early.c — derive the early-MMIO handoff
 * from the firmware device tree.
 *
 * The boot page tables map one contiguous run of 2 MiB Device blocks
 * (head.S) and the M1 runtime tree reproduces it (device_window.c).
 * On QEMU virt that window used to be compile-time [0x08000000,
 * 0x0a000000); here it is derived from the devices the kernel actually
 * touches through it: the early PL011 console and the GICv2 interrupt
 * controller (all reg pairs, covering GICD/GICC/v2m).
 *
 * Deliberately minimal (freestanding, no libc): a malformed or foreign
 * blob yields "not found" and the kernel falls back to its compile-time
 * defaults. Per the device-tree spec, a node's reg is interpreted with
 * the PARENT's #address-cells/#size-cells, so the cells values are
 * inherited down the tree as nodes open.
 */
#include <stdint.h>

#include "loader.h"

#define FDT_MAGIC           UINT32_C(0xd00dfeed)
#define FDT_BEGIN_NODE      UINT32_C(0x00000001)
#define FDT_END_NODE        UINT32_C(0x00000002)
#define FDT_PROP            UINT32_C(0x00000003)
#define FDT_NOP             UINT32_C(0x00000004)
#define FDT_END             UINT32_C(0x00000009)
#define FDT_HEADER_SIZE     40
#define FDT_MIN_VERSION     3   /* property names live in the strings block */

#define MMIO_BLOCK_SHIFT    21
#define MMIO_BLOCK_BYTES    (UINT64_C(1) << MMIO_BLOCK_SHIFT)
#define MMIO_MAX_BLOCKS     1024
#define FDT_MAX_DEPTH       63

static uint32_t load_be32(const void *address)
{
    const uint8_t *bytes = (const uint8_t *)address;

    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static uint64_t load_be_cells(const uint32_t *cells, unsigned count)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < count; ++i)
        value = (value << 32) | load_be32(&cells[i]);
    return value;
}

/* Bounds-safe property-name equality: `name` must be `lit` followed by
 * a NUL, without reading past `limit`. */
static int prop_name_is(const uint8_t *name, const uint8_t *limit,
                        const char *lit)
{
    unsigned i;

    for (i = 0; lit[i] != '\0'; ++i) {
        if (name + i >= limit || (char)name[i] != lit[i])
            return 0;
    }
    return name + i < limit && name[i] == 0;
}

/* The compatible value is a concatenation of NUL-terminated strings;
 * one of them must equal `name` exactly. */
static int compat_matches(const uint8_t *value, uint32_t len,
                          const char *name)
{
    uint32_t start = 0;
    uint32_t i;

    for (i = 0; i <= len; ++i) {
        if (i == len || value[i] == 0) {
            uint32_t n = 0;

            while (start + n < i && name[n] != '\0' &&
                   value[start + n] == (uint8_t)name[n])
                n++;
            if (start + n == i && name[n] == '\0')
                return 1;
            start = i + 1;
        }
    }
    return 0;
}

struct early_mmio_scan {
    uint64_t window_base;      /* byte-aligned low bound, pre-snap */
    uint64_t window_end;       /* byte-aligned high bound, pre-snap */
    uint64_t console_base;
    int have_device;
    int have_console;
};

static void scan_add_reg(struct early_mmio_scan *scan, uint64_t base,
                         uint64_t size)
{
    if (size == 0)
        return;
    if (!scan->have_device || base < scan->window_base)
        scan->window_base = base;
    if (!scan->have_device || base + size > scan->window_end)
        scan->window_end = base + size;
    scan->have_device = 1;
}

/* Fold one closed node into the scan. The window union consumes every
 * reg pair of the matched device (GICD/GICC/v2m; the UART's own range
 * too — the window must cover the console). The UART additionally
 * records its first base as the early console PA. */
static void scan_close_node(struct early_mmio_scan *scan,
                            const uint8_t *compatible, uint32_t compat_len,
                            const uint32_t *reg, uint32_t reg_len,
                            unsigned address_cells, unsigned size_cells)
{
    const uint32_t *cell;
    uint32_t remain;
    int is_uart = 0;
    int is_gic = 0;

    if (!compatible)
        return;
    is_uart = compat_matches(compatible, compat_len, "arm,pl011");
    is_gic = compat_matches(compatible, compat_len, "arm,cortex-a15-gic") ||
             compat_matches(compatible, compat_len, "arm,gic-400");
    if ((!is_uart && !is_gic) || !reg || address_cells == 0 ||
        size_cells == 0)
        return;

    cell = reg;
    remain = reg_len;
    while (remain >= (address_cells + size_cells) * 4u) {
        uint64_t base = load_be_cells(cell, address_cells);
        uint64_t size = load_be_cells(cell + address_cells, size_cells);

        scan_add_reg(scan, base, size);
        if (is_uart && !scan->have_console && size != 0) {
            scan->console_base = base;
            scan->have_console = 1;
        }
        cell += address_cells + size_cells;
        remain -= (address_cells + size_cells) * 4u;
    }
}

int aarch64_fdt_early_mmio(const void *fdt, uint32_t fdt_size,
                           struct aarch64_early_mmio_handoff *out)
{
    const uint8_t *blob = (const uint8_t *)fdt;
    const uint8_t *blob_end = blob + fdt_size;
    struct early_mmio_scan scan;
    unsigned address_cells[FDT_MAX_DEPTH + 1];
    unsigned size_cells[FDT_MAX_DEPTH + 1];
    /* Per-depth captured node props: a node's compatible/reg must
     * survive its children (e.g. the GIC's v2m frame subnode), so a
     * single slot is not enough — each depth keeps its own. */
    const uint8_t *node_compatible[FDT_MAX_DEPTH + 1];
    uint32_t node_compat_len[FDT_MAX_DEPTH + 1];
    const uint32_t *node_reg[FDT_MAX_DEPTH + 1];
    uint32_t node_reg_len[FDT_MAX_DEPTH + 1];
    uint32_t totalsize, off_struct, size_struct, off_strings;
    uint32_t version, cursor, struct_end;
    unsigned depth = 0;

    if (!out || !blob || fdt_size < FDT_HEADER_SIZE)
        return 0;
    if (load_be32(blob) != FDT_MAGIC)
        return 0;
    totalsize = load_be32(blob + 4);
    off_struct = load_be32(blob + 8);
    off_strings = load_be32(blob + 12);
    version = load_be32(blob + 20);
    size_struct = load_be32(blob + 36);
    if (totalsize == 0 || totalsize > fdt_size ||
        off_struct >= fdt_size || size_struct == 0 ||
        off_struct + size_struct > fdt_size ||
        off_strings >= fdt_size || version < FDT_MIN_VERSION)
        return 0;
    struct_end = off_struct + size_struct;

    for (unsigned i = 0; i < sizeof(scan); ++i)
        ((uint8_t *)&scan)[i] = 0;
    address_cells[0] = 2;
    size_cells[0] = 2;

    cursor = off_struct;
    int saw_end = 0;
    while (cursor + 4 <= struct_end) {
        uint32_t token = load_be32(blob + cursor);
        cursor += 4;

        if (token == FDT_END) {
            saw_end = 1;
            break;
        }
        if (token == FDT_NOP)
            continue;

        if (token == FDT_BEGIN_NODE) {
            /* Skip the NUL-terminated name (aligned up to 4). */
            while (cursor < struct_end && blob[cursor] != 0)
                cursor++;
            cursor++;
            cursor = (cursor + 3u) & ~UINT32_C(3);
            if (depth >= FDT_MAX_DEPTH)
                return 0;
            depth++;
            address_cells[depth] = address_cells[depth - 1];
            size_cells[depth] = size_cells[depth - 1];
            node_compatible[depth] = 0;
            node_compat_len[depth] = 0;
            node_reg[depth] = 0;
            node_reg_len[depth] = 0;
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0)
                return 0;
            /* reg is sized by the PARENT's cells (depth-1 values). */
            scan_close_node(&scan, node_compatible[depth],
                            node_compat_len[depth], node_reg[depth],
                            node_reg_len[depth],
                            address_cells[depth - 1], size_cells[depth - 1]);
            depth--;
            continue;
        }
        if (token == FDT_PROP) {
            uint32_t len, nameoff, name_start;
            const uint8_t *name, *value;

            if (cursor + 8 > struct_end)
                return 0;
            len = load_be32(blob + cursor);
            nameoff = load_be32(blob + cursor + 4);
            cursor += 8;
            if (len > struct_end - cursor)
                return 0;
            name_start = off_strings + nameoff;
            if (name_start >= fdt_size)
                return 0;
            name = blob + name_start;
            value = blob + cursor;

            if (prop_name_is(name, blob_end, "#address-cells")) {
                if (len == 4)
                    address_cells[depth] = load_be32(value);
            } else if (prop_name_is(name, blob_end, "#size-cells")) {
                if (len == 4)
                    size_cells[depth] = load_be32(value);
            } else if (prop_name_is(name, blob_end, "compatible")) {
                node_compatible[depth] = value;
                node_compat_len[depth] = len;
            } else if (prop_name_is(name, blob_end, "reg")) {
                node_reg[depth] = (const uint32_t *)value;
                node_reg_len[depth] = len;
            }

            cursor = (cursor + len + 3u) & ~UINT32_C(3);
            continue;
        }
        return 0;   /* unknown token — treat the blob as foreign */
    }
    if (!saw_end)
        return 0;   /* unterminated structure block */

    if (!scan.have_device)
        return 0;
    {
        uint64_t base = scan.window_base & ~(MMIO_BLOCK_BYTES - 1);
        uint64_t end = (scan.window_end + MMIO_BLOCK_BYTES - 1) &
                       ~(MMIO_BLOCK_BYTES - 1);
        uint64_t blocks = (end - base) >> MMIO_BLOCK_SHIFT;

        if (blocks == 0 || blocks > MMIO_MAX_BLOCKS)
            return 0;
        out->console_base = scan.have_console ? scan.console_base : 0;
        out->device_base = base;
        out->device_blocks = (uint32_t)blocks;
        out->reserved = 0;
    }
    return 1;
}
