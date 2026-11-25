/* boot/uefi/arch/aarch64/fdt_early.c — derive the early-MMIO handoff
 * from the firmware device tree.
 *
 * Shares single-step bus translation (fdt_address.h) with the kernel's
 * DTB parser.  Provides both the legacy pure-extraction helper
 * aarch64_fdt_early_mmio() and the boot-time policy entry
 * aarch64_fdt_boot_scan().
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "loader.h"
#include "../../../../kernel/include/arch/aarch64/fdt_address.h"

#define FDT_MAGIC           UINT32_C(0xd00dfeed)
#define FDT_BEGIN_NODE      UINT32_C(0x00000001)
#define FDT_END_NODE        UINT32_C(0x00000002)
#define FDT_PROP            UINT32_C(0x00000003)
#define FDT_NOP             UINT32_C(0x00000004)
#define FDT_END             UINT32_C(0x00000009)
#define FDT_HEADER_SIZE     40u
#define FDT_MIN_VERSION     3u

#define MMIO_BLOCK_SHIFT    21u
#define MMIO_BLOCK_BYTES    (UINT64_C(1) << MMIO_BLOCK_SHIFT)
#define MMIO_MAX_BLOCKS     1024u
#define FDT_MAX_DEPTH       63u

#define PL011_MIN_REG_SPAN  UINT64_C(0x48)
#define GIC_MIN_REG_SPAN    UINT64_C(0x1000)
#define FDT_PATH_MAX        256u

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

static int node_is_enabled(const uint8_t *status, uint32_t len)
{
    return !status || compat_matches(status, len, "okay") ||
           compat_matches(status, len, "ok");
}

static bool cstr_eq_n(const char *a, size_t a_len, const char *b)
{
    size_t i = 0;

    while (i < a_len && b[i] != '\0') {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return i == a_len && b[i] == '\0';
}

static size_t strlen_bounded(const char *s, uint32_t max_bytes)
{
    size_t n = 0;

    while (n < max_bytes && s[n] != '\0')
        n++;
    return n;
}

struct fdt_header_info {
    uint32_t off_struct;
    uint32_t struct_end;
    uint32_t off_strings;
};

static bool fdt_validate_header(const uint8_t *blob, uint32_t fdt_size,
                                struct fdt_header_info *hdr)
{
    uint32_t totalsize, off_struct, size_struct, off_strings, version;

    if (!blob || fdt_size < FDT_HEADER_SIZE)
        return false;
    if (load_be32(blob) != FDT_MAGIC)
        return false;
    totalsize = load_be32(blob + 4);
    off_struct = load_be32(blob + 8);
    off_strings = load_be32(blob + 12);
    version = load_be32(blob + 20);
    size_struct = load_be32(blob + 36);
    if (totalsize == 0 || totalsize > fdt_size ||
        off_struct >= fdt_size || size_struct == 0 ||
        size_struct > fdt_size - off_struct ||
        off_strings >= fdt_size || version < FDT_MIN_VERSION)
        return false;
    hdr->off_struct = off_struct;
    hdr->struct_end = off_struct + size_struct;
    hdr->off_strings = off_strings;
    return true;
}

struct early_mmio_scan {
    uint64_t window_base;
    uint64_t window_end;
    uint64_t console_base;
    int have_device;
    int have_gic;
    int have_console;
    int console_enabled;
    int boot_mode;
    int have_target_path;
    char target_path[FDT_PATH_MAX];
};

struct fdt_ancestry {
    unsigned address_cells[FDT_MAX_DEPTH + 1];
    unsigned size_cells[FDT_MAX_DEPTH + 1];
    const uint8_t *compatible[FDT_MAX_DEPTH + 1];
    uint32_t compat_len[FDT_MAX_DEPTH + 1];
    const uint32_t *reg[FDT_MAX_DEPTH + 1];
    uint32_t reg_len[FDT_MAX_DEPTH + 1];
    const uint32_t *ranges[FDT_MAX_DEPTH + 1];
    uint32_t ranges_len[FDT_MAX_DEPTH + 1];
    bool has_ranges[FDT_MAX_DEPTH + 1];
    const uint8_t *status[FDT_MAX_DEPTH + 1];
    uint32_t status_len[FDT_MAX_DEPTH + 1];
    bool tree_enabled[FDT_MAX_DEPTH + 1];
    uint16_t path_len[FDT_MAX_DEPTH + 1];
    char path[FDT_PATH_MAX];
};

static bool scan_add_reg(struct early_mmio_scan *scan, uint64_t base,
                         uint64_t size)
{
    if (size == 0 || size > UINT64_MAX - base)
        return false;
    if (!scan->have_device || base < scan->window_base)
        scan->window_base = base;
    if (!scan->have_device || base + size > scan->window_end)
        scan->window_end = base + size;
    scan->have_device = 1;
    return true;
}

static bool to_cpu_pa_span(const struct fdt_ancestry *an, unsigned depth,
                           uint64_t size, uint64_t *addr)
{
    for (unsigned d = depth - 1; d > 1; --d) {
        struct aarch64_fdt_bus bus = {
            .ranges = (const uint8_t *)an->ranges[d],
            .ranges_len = an->ranges_len[d],
            .child_cells = an->address_cells[d],
            .parent_cells = an->address_cells[d - 1],
            .size_cells = an->size_cells[d],
            .has_ranges = an->has_ranges[d],
        };
        if (!aarch64_fdt_translate_reg_step(&bus, size, addr))
            return false;
    }
    return true;
}

/* Legacy translation helper for aarch64_fdt_early_mmio(): preserves
 * transparent-when-missing-ranges behavior for legacy unit fixtures while
 * using the shared step helper whenever ranges is present. */
static bool to_cpu_pa_legacy(const struct fdt_ancestry *an, unsigned depth,
                             uint64_t size, uint64_t *addr)
{
    for (unsigned d = depth - 1; d > 1; --d) {
        if (!an->has_ranges[d] || an->ranges_len[d] == 0)
            continue;
        struct aarch64_fdt_bus bus = {
            .ranges = (const uint8_t *)an->ranges[d],
            .ranges_len = an->ranges_len[d],
            .child_cells = an->address_cells[d],
            .parent_cells = an->address_cells[d - 1],
            .size_cells = an->size_cells[d],
            .has_ranges = true,
        };
        if (!aarch64_fdt_translate_reg_step(&bus, size, addr))
            return false;
    }
    return true;
}

static void scan_close_node_legacy(struct early_mmio_scan *scan,
                                   const struct fdt_ancestry *an,
                                   unsigned depth)
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
        if (size == 0 || !to_cpu_pa_legacy(an, depth, size, &base))
            continue;
        if (!scan_add_reg(scan, base, size))
            continue;
        if (is_uart) {
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

static void scan_close_node_boot(struct early_mmio_scan *scan,
                                 const struct fdt_ancestry *an,
                                 unsigned depth, uint64_t firmware_console_pa)
{
    unsigned address_cells = an->address_cells[depth - 1];
    unsigned size_cells = an->size_cells[depth - 1];
    const uint8_t *compatible = an->compatible[depth];
    uint32_t compat_len = an->compat_len[depth];
    const uint32_t *reg = an->reg[depth];
    uint32_t reg_len = an->reg_len[depth];
    uint32_t stride;
    bool on;
    bool is_uart;
    bool is_gic;

    if (!compatible || !reg || (address_cells != 1u && address_cells != 2u) ||
        (size_cells != 1u && size_cells != 2u))
        return;

    on = an->tree_enabled[depth - 1] &&
         node_is_enabled(an->status[depth], an->status_len[depth]);
    if (!on)
        return;

    stride = (address_cells + size_cells) * 4u;
    if (reg_len == 0u || (reg_len % stride) != 0u)
        return;

    is_gic = compat_matches(compatible, compat_len, "arm,cortex-a15-gic") ||
             compat_matches(compatible, compat_len, "arm,gic-400");
    if (is_gic && !scan->have_gic) {
        uint32_t pairs = reg_len / stride;
        uint64_t bases[8];
        uint64_t sizes[8];

        if (pairs < 2u || pairs > 8u)
            return;
        for (uint32_t i = 0u; i < pairs; i++) {
            const uint32_t *cell = reg + i * (address_cells + size_cells);
            uint64_t b = load_be_cells(cell, address_cells);
            uint64_t s = load_be_cells(cell + address_cells, size_cells);
            uint64_t min_span = (i < 2u) ? GIC_MIN_REG_SPAN : UINT64_C(1);

            if (s < min_span || !to_cpu_pa_span(an, depth, s, &b) || b == 0u)
                return;
            bases[i] = b;
            sizes[i] = s;
        }
        for (uint32_t i = 0u; i < pairs; i++) {
            if (!scan_add_reg(scan, bases[i], sizes[i]))
                return;
        }
        scan->have_gic = 1;
        return;
    }

    is_uart = compat_matches(compatible, compat_len, "arm,pl011");
    if (is_uart && firmware_console_pa == 0u && !scan->have_console) {
        uint64_t base = load_be_cells(reg, address_cells);
        uint64_t size = load_be_cells(reg + address_cells, size_cells);

        if (scan->have_target_path) {
            if (!cstr_eq_n(an->path, an->path_len[depth], scan->target_path))
                return;
        }
        if (size < PL011_MIN_REG_SPAN ||
            !to_cpu_pa_span(an, depth, size, &base) || base == 0u)
            return;
        if (!scan_add_reg(scan, base, size))
            return;
        scan->console_base = base;
        scan->have_console = 1;
        scan->console_enabled = 1;
    }
}

static bool resolve_chosen_and_root(
    const uint8_t *blob, uint32_t fdt_size, const struct fdt_header_info *hdr,
    const char *required_compatible, bool resolve_stdout,
    bool *platform_mismatch, bool *stdout_broken,
    bool *has_target_path, char target_path[FDT_PATH_MAX])
{
    const uint8_t *blob_end = blob + fdt_size;
    uint32_t cursor = hdr->off_struct;
    unsigned depth = 0;
    int saw_end = 0;
    bool root_compat_checked = false;
    bool in_chosen = false;
    bool in_aliases = false;
    const char *stdout_raw = NULL;
    uint32_t stdout_len = 0;
    const char *alias_match = NULL;
    uint32_t alias_match_len = 0;

    *platform_mismatch = false;
    *stdout_broken = false;
    *has_target_path = false;

    while (cursor + 4u <= hdr->struct_end) {
        uint32_t token = load_be32(blob + cursor);
        cursor += 4u;

        if (token == FDT_END) {
            saw_end = 1;
            break;
        }
        if (token == FDT_NOP)
            continue;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)(blob + cursor);
            while (cursor < hdr->struct_end && blob[cursor] != 0)
                cursor++;
            if (cursor >= hdr->struct_end)
                return false;
            cursor++;
            cursor = (cursor + 3u) & ~UINT32_C(3);
            if (depth >= FDT_MAX_DEPTH)
                return false;
            depth++;
            if (depth == 2u) {
                in_chosen = cstr_eq_n(name, strlen_bounded(name, hdr->struct_end), "chosen");
                in_aliases = cstr_eq_n(name, strlen_bounded(name, hdr->struct_end), "aliases");
            }
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0u)
                return false;
            if (depth == 2u) {
                in_chosen = false;
                in_aliases = false;
            }
            depth--;
            continue;
        }
        if (token == FDT_PROP) {
            uint32_t len, nameoff, name_start;
            const uint8_t *name, *value;

            if (cursor + 8u > hdr->struct_end)
                return false;
            len = load_be32(blob + cursor);
            nameoff = load_be32(blob + cursor + 4u);
            cursor += 8u;
            if (len > hdr->struct_end - cursor)
                return false;
            name_start = hdr->off_strings + nameoff;
            if (name_start >= fdt_size)
                return false;
            name = blob + name_start;
            value = blob + cursor;

            if (depth == 1u && prop_name_is(name, blob_end, "compatible")) {
                root_compat_checked = true;
                if (required_compatible &&
                    !compat_matches(value, len, required_compatible)) {
                    *platform_mismatch = true;
                    return true;
                }
            } else if (resolve_stdout && in_chosen && depth == 2u &&
                       prop_name_is(name, blob_end, "stdout-path")) {
                if (len == 0u || value[len - 1u] != 0) {
                    *stdout_broken = true;
                    return true;
                }
                stdout_raw = (const char *)value;
                stdout_len = len - 1u;
            }

            cursor = (cursor + len + 3u) & ~UINT32_C(3);
            continue;
        }
        return false;
    }

    if (!saw_end || depth != 0u)
        return false;
    if (required_compatible && !root_compat_checked) {
        *platform_mismatch = true;
        return true;
    }

    if (!resolve_stdout || !stdout_raw)
        return true;

    /* Strip :options suffix */
    uint32_t id_len = 0u;
    while (id_len < stdout_len && stdout_raw[id_len] != ':')
        id_len++;
    if (id_len == 0u) {
        *stdout_broken = true;
        return true;
    }

    if (stdout_raw[0] == '/') {
        if (id_len >= FDT_PATH_MAX) {
            *stdout_broken = true;
            return true;
        }
        for (uint32_t i = 0u; i < id_len; i++)
            target_path[i] = stdout_raw[i];
        target_path[id_len] = '\0';
        *has_target_path = true;
        return true;
    }

    /* Alias lookup in /aliases */
    cursor = hdr->off_struct;
    depth = 0u;
    in_aliases = false;
    while (cursor + 4u <= hdr->struct_end) {
        uint32_t token = load_be32(blob + cursor);
        cursor += 4u;
        if (token == FDT_END)
            break;
        if (token == FDT_NOP)
            continue;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)(blob + cursor);
            size_t nlen = 0;
            while (cursor < hdr->struct_end && blob[cursor] != 0) {
                cursor++;
                nlen++;
            }
            cursor++;
            cursor = (cursor + 3u) & ~UINT32_C(3);
            depth++;
            if (depth == 2u)
                in_aliases = cstr_eq_n(name, nlen, "aliases");
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 2u)
                in_aliases = false;
            depth--;
            continue;
        }
        if (token == FDT_PROP) {
            uint32_t len = load_be32(blob + cursor);
            uint32_t nameoff = load_be32(blob + cursor + 4u);
            const uint8_t *name = blob + hdr->off_strings + nameoff;
            const uint8_t *value = blob + cursor + 8u;
            cursor += 8u;
            if (in_aliases && depth == 2u) {
                uint32_t nl = 0u;
                while (name + nl < blob_end && name[nl] != 0)
                    nl++;
                if (nl == id_len && cstr_eq_n(stdout_raw, id_len, (const char *)name)) {
                    alias_match = (const char *)value;
                    alias_match_len = len;
                    break;
                }
            }
            cursor = (cursor + len + 3u) & ~UINT32_C(3);
        }
    }

    if (!alias_match || alias_match_len < 2u ||
        alias_match[alias_match_len - 1u] != '\0' ||
        alias_match[0] != '/' || alias_match_len > FDT_PATH_MAX) {
        *stdout_broken = true;
        return true;
    }
    for (uint32_t i = 0u; i < alias_match_len; i++)
        target_path[i] = alias_match[i];
    *has_target_path = true;
    return true;
}

static bool walk_fdt_devices(const uint8_t *blob, uint32_t fdt_size,
                             const struct fdt_header_info *hdr,
                             struct early_mmio_scan *scan,
                             uint64_t firmware_console_pa)
{
    const uint8_t *blob_end = blob + fdt_size;
    struct fdt_ancestry an;
    uint32_t cursor = hdr->off_struct;
    unsigned depth = 0;
    int saw_end = 0;

    an.address_cells[0] = 2u;
    an.size_cells[0] = 2u;
    an.tree_enabled[0] = true;
    an.path_len[0] = 0u;

    while (cursor + 4u <= hdr->struct_end) {
        uint32_t token = load_be32(blob + cursor);
        cursor += 4u;

        if (token == FDT_END) {
            saw_end = 1;
            break;
        }
        if (token == FDT_NOP)
            continue;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)(blob + cursor);
            uint32_t name_len = 0u;

            while (cursor < hdr->struct_end && blob[cursor] != 0) {
                cursor++;
                name_len++;
            }
            if (cursor >= hdr->struct_end)
                return false;
            cursor++;
            cursor = (cursor + 3u) & ~UINT32_C(3);
            if (depth >= FDT_MAX_DEPTH)
                return false;

            if (depth > 0u) {
                an.tree_enabled[depth] = an.tree_enabled[depth - 1u] &&
                    node_is_enabled(an.status[depth], an.status_len[depth]);
            }

            depth++;
            an.address_cells[depth] = an.address_cells[depth - 1u];
            an.size_cells[depth] = an.size_cells[depth - 1u];
            an.compatible[depth] = NULL;
            an.compat_len[depth] = 0u;
            an.reg[depth] = NULL;
            an.reg_len[depth] = 0u;
            an.ranges[depth] = NULL;
            an.ranges_len[depth] = 0u;
            an.has_ranges[depth] = false;
            an.status[depth] = NULL;
            an.status_len[depth] = 0u;
            an.tree_enabled[depth] = an.tree_enabled[depth - 1u];

            if (depth == 1u) {
                an.path[0] = '/';
                an.path_len[1] = 1u;
            } else {
                uint16_t base_len = an.path_len[depth - 1u];
                uint16_t pos = base_len;
                if (pos > 1u && pos < FDT_PATH_MAX - 1u)
                    an.path[pos++] = '/';
                for (uint32_t i = 0u; i < name_len && pos < FDT_PATH_MAX - 1u; i++)
                    an.path[pos++] = name[i];
                an.path_len[depth] = pos;
            }
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth == 0u)
                return false;
            if (scan->boot_mode)
                scan_close_node_boot(scan, &an, depth, firmware_console_pa);
            else
                scan_close_node_legacy(scan, &an, depth);
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32_t len, nameoff, name_start;
            const uint8_t *name, *value;

            if (cursor + 8u > hdr->struct_end)
                return false;
            len = load_be32(blob + cursor);
            nameoff = load_be32(blob + cursor + 4u);
            cursor += 8u;
            if (len > hdr->struct_end - cursor)
                return false;
            name_start = hdr->off_strings + nameoff;
            if (name_start >= fdt_size)
                return false;
            name = blob + name_start;
            value = blob + cursor;

            if (prop_name_is(name, blob_end, "#address-cells")) {
                if (len == 4u)
                    an.address_cells[depth] = load_be32(value);
            } else if (prop_name_is(name, blob_end, "#size-cells")) {
                if (len == 4u)
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
                an.has_ranges[depth] = true;
            } else if (prop_name_is(name, blob_end, "status")) {
                an.status[depth] = value;
                an.status_len[depth] = len;
            }

            cursor = (cursor + len + 3u) & ~UINT32_C(3);
            continue;
        }

        return false;
    }

    return saw_end && depth == 0u;
}

static bool finalize_window(const struct early_mmio_scan *scan,
                            uint64_t console_base,
                            struct aarch64_early_mmio_handoff *out)
{
    uint64_t base = scan->window_base & ~(MMIO_BLOCK_BYTES - 1u);
    uint64_t end = (scan->window_end + MMIO_BLOCK_BYTES - 1u) &
                   ~(MMIO_BLOCK_BYTES - 1u);
    uint64_t blocks;

    if (!scan->have_device || end <= base)
        return false;
    blocks = (end - base) >> MMIO_BLOCK_SHIFT;
    if (blocks == 0u || blocks > MMIO_MAX_BLOCKS)
        return false;
    out->console_base = console_base;
    out->device_base = base;
    out->device_blocks = (uint32_t)blocks;
    out->reserved = 0u;
    return true;
}

int aarch64_fdt_early_mmio(const void *fdt, uint32_t fdt_size,
                           struct aarch64_early_mmio_handoff *out)
{
    struct fdt_header_info hdr;
    struct early_mmio_scan scan;

    if (!out || !fdt_validate_header((const uint8_t *)fdt, fdt_size, &hdr))
        return 0;

    for (unsigned i = 0; i < sizeof(scan); ++i)
        ((uint8_t *)&scan)[i] = 0;
    scan.boot_mode = 0;

    if (!walk_fdt_devices((const uint8_t *)fdt, fdt_size, &hdr, &scan, 0u))
        return 0;

    return finalize_window(&scan, scan.have_console ? scan.console_base : 0u,
                           out) ? 1 : 0;
}

enum aarch64_fdt_scan_result aarch64_fdt_boot_scan(
    const void *blob, uint32_t size, const char *required_compatible,
    uint64_t firmware_console_pa, struct aarch64_early_mmio_handoff *out)
{
    struct fdt_header_info hdr;
    struct early_mmio_scan scan;
    bool platform_mismatch = false;
    bool stdout_broken = false;
    bool has_target = false;

    if (!out || !fdt_validate_header((const uint8_t *)blob, size, &hdr))
        return AARCH64_FDT_SCAN_INVALID;

    for (unsigned i = 0; i < sizeof(scan); ++i)
        ((uint8_t *)&scan)[i] = 0;
    scan.boot_mode = 1;

    if (!resolve_chosen_and_root((const uint8_t *)blob, size, &hdr,
                                 required_compatible,
                                 firmware_console_pa == 0u,
                                 &platform_mismatch, &stdout_broken,
                                 &has_target, scan.target_path))
        return AARCH64_FDT_SCAN_INVALID;

    if (platform_mismatch)
        return AARCH64_FDT_SCAN_PLATFORM_MISMATCH;
    if (stdout_broken)
        return AARCH64_FDT_SCAN_MISSING_MMIO;
    scan.have_target_path = has_target ? 1 : 0;

    if (!walk_fdt_devices((const uint8_t *)blob, size, &hdr, &scan,
                          firmware_console_pa))
        return AARCH64_FDT_SCAN_INVALID;

    if (!scan.have_gic)
        return AARCH64_FDT_SCAN_MISSING_MMIO;
    if (firmware_console_pa == 0u && !scan.have_console)
        return AARCH64_FDT_SCAN_MISSING_MMIO;

    if (!finalize_window(&scan,
                         firmware_console_pa != 0u ? firmware_console_pa
                                                   : scan.console_base,
                         out))
        return AARCH64_FDT_SCAN_MISSING_MMIO;

    return AARCH64_FDT_SCAN_OK;
}
