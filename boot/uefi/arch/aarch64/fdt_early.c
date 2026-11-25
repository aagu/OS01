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
 *
 * Devices may sit on nested buses (BCM2712 keeps its PL011 and GIC-400
 * under /soc): every enclosing bus's ranges property translates a child
 * address one level up, and a bus without ranges is transparent. Only
 * enabled nodes are used, with one exception spelled out in
 * scan_close_node().
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

/* status is absent, or is exactly okay/ok: the firmware left the node on. */
static int node_is_enabled(const uint8_t *status, uint32_t len)
{
    return !status || compat_matches(status, len, "okay") ||
           compat_matches(status, len, "ok");
}

struct early_mmio_scan {
    uint64_t window_base;      /* byte-aligned low bound, pre-snap */
    uint64_t window_end;       /* byte-aligned high bound, pre-snap */
    uint64_t console_base;
    int have_device;
    int have_console;
    int console_enabled;       /* the chosen console came from an enabled node */
};

/* Every open node's cells and the properties the close hook needs, indexed
 * by depth (1 = root). A node's reg and ranges are encoded with the cells
 * of the node above it, which is what translating an address walks. */
struct fdt_ancestry {
    unsigned address_cells[FDT_MAX_DEPTH + 1];
    unsigned size_cells[FDT_MAX_DEPTH + 1];
    const uint8_t *compatible[FDT_MAX_DEPTH + 1];
    uint32_t compat_len[FDT_MAX_DEPTH + 1];
    const uint32_t *reg[FDT_MAX_DEPTH + 1];
    uint32_t reg_len[FDT_MAX_DEPTH + 1];
    const uint32_t *ranges[FDT_MAX_DEPTH + 1];
    uint32_t ranges_len[FDT_MAX_DEPTH + 1];
    const uint8_t *status[FDT_MAX_DEPTH + 1];
    uint32_t status_len[FDT_MAX_DEPTH + 1];
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

/* Translate `addr`, a bus address in the parent space of the node at
 * `depth`, into the CPU physical view: each enclosing bus with a non-empty
 * ranges remaps it through its first covering entry, buses without one are
 * transparent, and the root's own ranges is never applied. Returns 0 when
 * a bus cannot be decoded or no entry covers the address. */
static int to_cpu_pa(const struct fdt_ancestry *an, unsigned depth,
                     uint64_t *addr)
{
    unsigned d;

    for (d = depth - 1; d > 1; --d) {
        const uint8_t *ranges = (const uint8_t *)an->ranges[d];
        uint32_t len = an->ranges_len[d];
        unsigned child_cells = an->address_cells[d];
        unsigned size_cells = an->size_cells[d];
        unsigned parent_cells = an->address_cells[d - 1];
        uint32_t stride;
        uint32_t off;
        int hit = 0;

        if (!len)
            continue;
        if (child_cells < 1 || child_cells > 2 ||
            size_cells < 1 || size_cells > 2 ||
            parent_cells < 1 || parent_cells > 2)
            return 0;
        /* One entry = <child-addr: bus cells> <parent-addr: parent cells>
         * <size: bus cells>. */
        stride = (child_cells + parent_cells + size_cells) * 4u;
        if (len % stride != 0u)
            return 0;
        for (off = 0; off < len && !hit; off += stride) {
            const uint32_t *entry = (const uint32_t *)(ranges + off);
            uint64_t child = load_be_cells(entry, child_cells);
            uint64_t size = load_be_cells(entry + child_cells + parent_cells,
                                          size_cells);
            uint64_t parent;
            uint64_t delta;

            /* Containment without computing child + size (may overflow). */
            if (*addr < child || *addr - child >= size)
                continue;
            parent = load_be_cells(entry + child_cells, parent_cells);
            delta = *addr - child;
            if (parent > (~UINT64_C(0)) - delta)
                return 0;
            *addr = parent + delta;
            hit = 1;
        }
        if (!hit)
            return 0;
    }
    return 1;
}

/* Fold one closed node into the scan. The window union consumes every
 * reg pair of the matched device (GICD/GICC/v2m; the UART's own range
 * too — the window must cover the console). The UART additionally
 * records its first base as the early console PA. */
static void scan_close_node(struct early_mmio_scan *scan,
                            const struct fdt_ancestry *an, unsigned depth)
{
    unsigned address_cells = an->address_cells[depth - 1];
    unsigned size_cells = an->size_cells[depth - 1];
    const uint8_t *compatible = an->compatible[depth];
    uint32_t compat_len = an->compat_len[depth];
    const uint32_t *reg = an->reg[depth];
    uint32_t reg_len = an->reg_len[depth];
    const uint32_t *cell;
    uint32_t remain;
    int is_uart = 0;
    int is_gic = 0;
    int on;

    if (!compatible)
        return;
    is_uart = compat_matches(compatible, compat_len, "arm,pl011");
    is_gic = compat_matches(compatible, compat_len, "arm,cortex-a15-gic") ||
             compat_matches(compatible, compat_len, "arm,gic-400");
    if ((!is_uart && !is_gic) || !reg || address_cells == 0 ||
        size_cells == 0)
        return;
    /* A node the firmware parked is absent — except that a disabled PL011
     * stands in as the console while nothing enabled has shown up, so the
     * kernel still gets a diagnostics port instead of printing into
     * nowhere. */
    on = node_is_enabled(an->status[depth], an->status_len[depth]);
    if (!on && (!is_uart || scan->have_console))
        return;

    cell = reg;
    remain = reg_len;
    while (remain >= (address_cells + size_cells) * 4u) {
        uint64_t base = load_be_cells(cell, address_cells);
        uint64_t size = load_be_cells(cell + address_cells, size_cells);

        cell += address_cells + size_cells;
        remain -= (address_cells + size_cells) * 4u;
        if (!to_cpu_pa(an, depth, &base))
            continue;   /* no ranges entry covers it: no CPU address */
        scan_add_reg(scan, base, size);
        if (is_uart && size != 0) {
            if (!scan->have_console) {
                scan->console_base = base;
                scan->have_console = 1;
                scan->console_enabled = on;
            } else if (on && !scan->console_enabled) {
                scan->console_base = base;
                scan->console_enabled = 1;
            }
        }
    }
}

int aarch64_fdt_early_mmio(const void *fdt, uint32_t fdt_size,
                           struct aarch64_early_mmio_handoff *out)
{
    const uint8_t *blob = (const uint8_t *)fdt;
    const uint8_t *blob_end = blob + fdt_size;
    struct early_mmio_scan scan;
    struct fdt_ancestry an;
    uint32_t totalsize, off_struct, size_struct, off_strings;
    uint32_t version, cursor, struct_end;
    unsigned depth = 0;
    int saw_end = 0;

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
    an.address_cells[0] = 2;
    an.size_cells[0] = 2;

    cursor = off_struct;
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
            an.address_cells[depth] = an.address_cells[depth - 1];
            an.size_cells[depth] = an.size_cells[depth - 1];
            an.compatible[depth] = 0;
            an.compat_len[depth] = 0;
            an.reg[depth] = 0;
            an.reg_len[depth] = 0;
            an.ranges[depth] = 0;
            an.ranges_len[depth] = 0;
            an.status[depth] = 0;
            an.status_len[depth] = 0;
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0)
                return 0;
            /* reg is sized by the PARENT's cells (depth-1 values). */
            scan_close_node(&scan, &an, depth);
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
                    an.address_cells[depth] = load_be32(value);
            } else if (prop_name_is(name, blob_end, "#size-cells")) {
                if (len == 4)
                    an.size_cells[depth] = load_be32(value);
            } else if (prop_name_is(name, blob_end, "compatible")) {
                an.compatible[depth] = value;
                an.compat_len[depth] = len;
            } else if (prop_name_is(name, blob_end, "reg")) {
                an.reg[depth] = (const uint32_t *)value;
                an.reg_len[depth] = len;
            } else if (prop_name_is(name, blob_end, "ranges")) {
                an.ranges[depth] = (const uint32_t *)value;
                an.ranges_len[depth] = len;
            } else if (prop_name_is(name, blob_end, "status")) {
                an.status[depth] = value;
                an.status_len[depth] = len;
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
