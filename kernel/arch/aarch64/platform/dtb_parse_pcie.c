/* Bounded, freestanding FDT v17 parser for AArch64 PCIe host bridges
 * (Phase 1: compatible = "pci-host-ecam-generic"). */
#include <arch/aarch64/pcie.h>
#include <arch/aarch64/fdt_address.h>
#include <arch/pci.h>
#include <log/log.h>

#define FDT_MAGIC UINT32_C(0xd00dfeed)
#define AARCH64_PCIE_FDT_MAX_DEPTH 8U

struct pcie_prop {
    uint32_t off;
    uint32_t len;
};

struct pcie_fdt_node {
    uint32_t name_off;
    uint32_t address_cells;
    uint32_t size_cells;
    uint32_t parent_address_cells;
    uint32_t parent_size_cells;
    bool has_address_cells;
    bool has_size_cells;
    struct pcie_prop reg;
    struct pcie_prop compatible;
    struct pcie_prop status;
    struct pcie_prop bus_range;
    struct pcie_prop pci_domain;
    struct pcie_prop ranges;
};

struct pcie_bus_frame {
    uint32_t ranges_off;
    uint32_t ranges_len;
    uint32_t address_cells;
    uint32_t size_cells;
    bool has_ranges;
};

static uint32_t pcie_be32(const uint8_t *p)
{
    return aarch64_fdt_be32(p);
}

static bool pcie_range_ok(uint32_t size, uint32_t off, uint32_t len)
{
    return off <= size && len <= size - off;
}

static void pcie_zero(void *dst, uint32_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)dst;
    while (size--) {
        *p++ = 0;
    }
}

static bool pcie_streq(const uint8_t *a, const char *b)
{
    while (*a && *b && *a == (uint8_t)*b) {
        ++a;
        ++b;
    }
    return *a == (uint8_t)*b;
}

static bool pcie_strings_valid(const uint8_t *b, struct pcie_prop p)
{
    return p.len > 0 && b[p.off + p.len - 1] == 0;
}

static bool pcie_prop_exact(const uint8_t *b, struct pcie_prop p, const char *s)
{
    uint32_t n = 0;
    while (s[n]) {
        ++n;
    }
    return p.off != 0 && p.len == n + 1U && pcie_streq(b + p.off, s);
}

static bool pcie_prop_contains(const uint8_t *b, struct pcie_prop p, const char *s)
{
    uint32_t i = 0;
    const uint8_t *d = b + p.off;
    if (!p.off) {
        return false;
    }
    while (i < p.len) {
        if (pcie_streq(d + i, s)) {
            return true;
        }
        while (i < p.len && d[i] != 0) {
            ++i;
        }
        ++i;
    }
    return false;
}

static bool pcie_node_enabled(const uint8_t *b, const struct pcie_fdt_node *n)
{
    return !n->status.off ||
           pcie_prop_exact(b, n->status, "okay") ||
           pcie_prop_exact(b, n->status, "ok");
}

static bool pcie_to_cpu_pa(const uint8_t *b, const struct pcie_bus_frame *stack,
                           uint32_t depth, uint64_t size, uint64_t *addr)
{
    for (uint32_t d = depth - 1U; d > 1U; --d) {
        const struct pcie_bus_frame *bus = &stack[d - 1U];
        struct aarch64_fdt_bus fbus = {
            .ranges = bus->has_ranges ? (b + bus->ranges_off) : NULL,
            .ranges_len = bus->ranges_len,
            .child_cells = bus->address_cells,
            .parent_cells = stack[d - 2U].address_cells,
            .size_cells = bus->size_cells,
            .has_ranges = bus->has_ranges,
        };
        if (!aarch64_fdt_translate_reg_step(&fbus, size, addr)) {
            return false;
        }
    }
    return true;
}

static bool pcie_domain_bus_conflicts(const struct aarch64_pcie_topology *out,
                                      uint16_t domain, uint8_t bus_start, uint8_t bus_end)
{
    for (uint32_t i = 0; i < out->root_count; ++i) {
        const struct aarch64_pcie_root_desc *r = &out->roots[i];
        if (!r->valid || r->domain != domain) {
            continue;
        }
        if (!(bus_end < r->bus_start || bus_start > r->bus_end)) {
            return true;
        }
    }
    return false;
}

static int pcie_prop_record(const uint8_t *b, struct pcie_fdt_node *n,
                            const uint8_t *name, struct pcie_prop p)
{
    struct pcie_prop *dst = NULL;
    bool is_str = false;

    if (pcie_streq(name, "#address-cells")) {
        if (p.len != 4U) {
            return -1;
        }
        n->address_cells = pcie_be32(b + p.off);
        n->has_address_cells = true;
        return 0;
    }
    if (pcie_streq(name, "#size-cells")) {
        if (p.len != 4U) {
            return -1;
        }
        n->size_cells = pcie_be32(b + p.off);
        n->has_size_cells = true;
        return 0;
    }

    if (pcie_streq(name, "reg")) {
        dst = &n->reg;
    } else if (pcie_streq(name, "bus-range")) {
        dst = &n->bus_range;
    } else if (pcie_streq(name, "linux,pci-domain")) {
        dst = &n->pci_domain;
    } else if (pcie_streq(name, "ranges")) {
        dst = &n->ranges;
    } else if (pcie_streq(name, "compatible")) {
        dst = &n->compatible;
        is_str = true;
    } else if (pcie_streq(name, "status")) {
        dst = &n->status;
        is_str = true;
    }

    if (is_str && !pcie_strings_valid(b, p)) {
        return -1;
    }
    if (dst) {
        if (dst->off) {
            return -1;
        }
        *dst = p;
    }
    return 0;
}

static void finish_pcie_node(const uint8_t *b, const struct pcie_fdt_node *n,
                             uint32_t depth, const struct pcie_bus_frame *stack,
                             struct aarch64_pcie_topology *out)
{
    if (!pcie_prop_contains(b, n->compatible, "pci-host-ecam-generic")) {
        return;
    }
    if (!pcie_node_enabled(b, n)) {
        return;
    }
    if (out->root_count >= AARCH64_PCIE_MAX_ROOTS) {
        log_warn("pcie-dtb: too many ECAM roots, skipping node\n");
        return;
    }

    uint32_t pac = n->parent_address_cells;
    uint32_t psc = n->parent_size_cells;
    if ((pac != 1U && pac != 2U) || (psc != 1U && psc != 2U)) {
        log_warn("pcie-dtb: invalid parent cells ac=%u sc=%u\n",
                 (unsigned int)pac, (unsigned int)psc);
        return;
    }

    /* Validate and translate ECAM reg */
    uint32_t reg_stride = (pac + psc) * 4U;
    if (!n->reg.off || n->reg.len < reg_stride || (n->reg.len % reg_stride) != 0U) {
        log_warn("pcie-dtb: missing or malformed reg property\n");
        return;
    }
    const uint8_t *rp = b + n->reg.off;
    uint64_t reg_cpu_base = aarch64_fdt_decode_cells(rp, pac);
    uint64_t reg_size = aarch64_fdt_decode_cells(rp + pac * 4U, psc);
    if (reg_size < 0x100000ULL || reg_cpu_base > UINT64_MAX - reg_size) {
        log_warn("pcie-dtb: invalid ECAM reg size=%lx\n", (unsigned long)reg_size);
        return;
    }
    if (!pcie_to_cpu_pa(b, stack, depth, reg_size, &reg_cpu_base)) {
        log_warn("pcie-dtb: failed to translate ECAM reg to CPU PA\n");
        return;
    }
    if ((reg_cpu_base & 0xFFFULL) != 0ULL) {
        log_warn("pcie-dtb: unaligned ECAM reg base=%lx\n", (unsigned long)reg_cpu_base);
        return;
    }

    /* Validate bus-range */
    uint8_t bus_start = 0;
    uint8_t bus_end = 0;
    if (n->bus_range.off) {
        if (n->bus_range.len != 8U) {
            log_warn("pcie-dtb: invalid bus-range length=%u\n", (unsigned int)n->bus_range.len);
            return;
        }
        uint32_t bs = pcie_be32(b + n->bus_range.off);
        uint32_t be = pcie_be32(b + n->bus_range.off + 4U);
        if (bs > be || be > 255U) {
            log_warn("pcie-dtb: invalid bus-range %u..%u\n", (unsigned int)bs, (unsigned int)be);
            return;
        }
        bus_start = (uint8_t)bs;
        bus_end = (uint8_t)be;
    } else {
        uint64_t ecam_buses = reg_size >> 20;
        if (ecam_buses > 256ULL) {
            ecam_buses = 256ULL;
        }
        bus_start = 0;
        bus_end = (uint8_t)(ecam_buses - 1ULL);
    }

    uint64_t active_size = ((uint64_t)(bus_end - bus_start + 1U)) << 20;
    if (active_size > reg_size || reg_cpu_base > UINT64_MAX - active_size) {
        log_warn("pcie-dtb: bus-range exceeds ECAM reg window\n");
        return;
    }

    /* Validate or allocate linux,pci-domain */
    uint16_t domain = 0;
    if (n->pci_domain.off) {
        if (n->pci_domain.len != 4U) {
            log_warn("pcie-dtb: invalid linux,pci-domain length\n");
            return;
        }
        uint32_t dv = pcie_be32(b + n->pci_domain.off);
        if (dv > 0xFFFFU) {
            log_warn("pcie-dtb: linux,pci-domain out of range %u\n", (unsigned int)dv);
            return;
        }
        domain = (uint16_t)dv;
        if (pcie_domain_bus_conflicts(out, domain, bus_start, bus_end)) {
            log_warn("pcie-dtb: duplicate domain=%u bus range %u..%u\n",
                     (unsigned int)domain, (unsigned int)bus_start, (unsigned int)bus_end);
            return;
        }
    } else {
        bool found_domain = false;
        for (uint32_t d = 0; d <= 0xFFFFU; ++d) {
            if (!pcie_domain_bus_conflicts(out, (uint16_t)d, bus_start, bus_end)) {
                domain = (uint16_t)d;
                found_domain = true;
                break;
            }
        }
        if (!found_domain) {
            log_warn("pcie-dtb: no free PCI domain\n");
            return;
        }
    }

    struct aarch64_pcie_root_desc desc;
    pcie_zero(&desc, sizeof(desc));
    desc.valid = true;
    desc.kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    desc.domain = domain;
    desc.bus_start = bus_start;
    desc.bus_end = bus_end;
    desc.reg_cpu_base = reg_cpu_base;
    desc.reg_size = reg_size;

    /* Decode 3-cell PCI ranges if present */
    if (n->ranges.off != 0U) {
        if (!n->has_address_cells || n->address_cells != 3U ||
            !n->has_size_cells || n->size_cells != 2U) {
            log_warn("pcie-dtb: PCI ranges requires #address-cells=<3> #size-cells=<2>\n");
            return;
        }
        uint32_t tuple_bytes = (3U + pac + 2U) * 4U;
        if ((n->ranges.len % tuple_bytes) != 0U) {
            log_warn("pcie-dtb: unaligned ranges property len=%u\n", (unsigned int)n->ranges.len);
            return;
        }
        for (uint32_t off = 0U; off < n->ranges.len; off += tuple_bytes) {
            const uint8_t *e = b + n->ranges.off + off;
            uint32_t phys_hi = pcie_be32(e);
            uint32_t space_code = (phys_hi >> 24) & 0x03U;
            if (space_code == 0U) {
                continue; /* Configuration space entry ignored */
            }
            uint8_t space = 0;
            if (space_code == 1U) {
                space = PCI_SPACE_IO;
            } else if (space_code == 2U) {
                space = PCI_SPACE_MMIO32;
            } else {
                space = PCI_SPACE_MMIO64;
            }
            bool prefetchable = ((phys_hi >> 30) & 1U) != 0U;
            uint64_t pci_base = aarch64_fdt_decode_cells(e + 4U, 2U);
            uint64_t cpu_base = aarch64_fdt_decode_cells(e + 12U, pac);
            uint64_t rsize = aarch64_fdt_decode_cells(e + 12U + pac * 4U, 2U);

            if (rsize == 0ULL) {
                continue;
            }
            if (pci_base > UINT64_MAX - rsize || cpu_base > UINT64_MAX - rsize) {
                log_warn("pcie-dtb: range entry overflow\n");
                return;
            }
            if (!pcie_to_cpu_pa(b, stack, depth, rsize, &cpu_base) ||
                cpu_base > UINT64_MAX - rsize) {
                log_warn("pcie-dtb: range ancestor translation failed\n");
                return;
            }
            if (desc.range_count >= AARCH64_PCIE_MAX_RANGES) {
                log_warn("pcie-dtb: ranges exceed max=%u\n", (unsigned int)AARCH64_PCIE_MAX_RANGES);
                return;
            }
            struct aarch64_pcie_range *dst = &desc.ranges[desc.range_count++];
            dst->space = space;
            dst->prefetchable = prefetchable;
            dst->pci_base = pci_base;
            dst->cpu_base = cpu_base;
            dst->size = rsize;
        }
    }

    out->roots[out->root_count++] = desc;
}

int aarch64_dtb_parse_pcie(const void *blob, uint32_t size,
                           struct aarch64_pcie_topology *out)
{
    if (out) {
        pcie_zero(out, sizeof(*out));
    }
    if (!blob || !out || size < 40U) {
        return -1;
    }

    const uint8_t *b = (const uint8_t *)blob;
    uint32_t total = pcie_be32(b + 4U);
    uint32_t so = pcie_be32(b + 8U);
    uint32_t no = pcie_be32(b + 12U);
    uint32_t ro = pcie_be32(b + 16U);
    uint32_t ns = pcie_be32(b + 32U);
    uint32_t ss = pcie_be32(b + 36U);

    if (pcie_be32(b) != FDT_MAGIC || total < 40U || total > size ||
        pcie_be32(b + 20U) < 17U || pcie_be32(b + 24U) > 17U ||
        so < 40U || no < 40U || ro < 40U || (so & 3U) || (ro & 7U) ||
        !pcie_range_ok(total, so, ss) || !pcie_range_ok(total, no, ns) ||
        !pcie_range_ok(total, ro, 16U)) {
        return -1;
    }
    if (so < no + ns && no < so + ss) {
        return -1;
    }

    uint32_t rp = ro;
    for (;;) {
        if (!pcie_range_ok(total, rp, 16U) ||
            (rp < so + ss && so < rp + 16U) ||
            (rp < no + ns && no < rp + 16U)) {
            return -1;
        }
        if (aarch64_fdt_decode_cells(b + rp, 2U) == 0ULL &&
            aarch64_fdt_decode_cells(b + rp + 8U, 2U) == 0ULL) {
            break;
        }
        rp += 16U;
    }

    struct pcie_bus_frame stack[AARCH64_PCIE_FDT_MAX_DEPTH];
    struct pcie_fdt_node current;
    pcie_zero(stack, sizeof(stack));
    pcie_zero(&current, sizeof(current));

    uint32_t pos = 0U;
    uint32_t depth = 0U;
    bool root = false;
    bool end = false;
    bool pending = false;
    const uint8_t *st = b + so;

    while (pos < ss) {
        if (!pcie_range_ok(ss, pos, 4U)) {
            pcie_zero(out, sizeof(*out));
            return -1;
        }
        uint32_t token = pcie_be32(st + pos);
        pos += 4U;

        if (token == 1U) {
            if (depth == AARCH64_PCIE_FDT_MAX_DEPTH || (!depth && root)) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            if (pending) {
                finish_pcie_node(b, &current, depth, stack, out);
                stack[depth - 1U].address_cells = current.address_cells;
                stack[depth - 1U].size_cells = current.size_cells;
                stack[depth - 1U].has_ranges = current.ranges.off != 0U;
                stack[depth - 1U].ranges_len = current.ranges.len;
                stack[depth - 1U].ranges_off = current.ranges.off;
            }
            uint32_t start = pos;
            while (pos < ss && st[pos]) {
                ++pos;
            }
            if (pos == ss) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            ++pos;
            uint32_t pad = (4U - (pos & 3U)) & 3U;
            if (!pcie_range_ok(ss, pos, pad)) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            pos += pad;

            pcie_zero(&current, sizeof(current));
            current.name_off = so + start;
            current.address_cells = 2U;
            current.size_cells = 1U;
            if (depth > 0U) {
                current.parent_address_cells = stack[depth - 1U].address_cells;
                current.parent_size_cells = stack[depth - 1U].size_cells;
            } else {
                if (st[start]) {
                    pcie_zero(out, sizeof(*out));
                    return -1;
                }
                root = true;
            }
            ++depth;
            pending = true;
        } else if (token == 2U) {
            if (!depth) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            if (pending) {
                finish_pcie_node(b, &current, depth, stack, out);
                pending = false;
            }
            --depth;
        } else if (token == 3U) {
            if (!depth || !pending || !pcie_range_ok(ss, pos, 8U)) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            uint32_t len = pcie_be32(st + pos);
            uint32_t nameoff = pcie_be32(st + pos + 4U);
            pos += 8U;
            if (!pcie_range_ok(ss, pos, len) || nameoff >= ns) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            uint32_t ni = nameoff;
            while (ni < ns && b[no + ni]) {
                ++ni;
            }
            if (ni == ns) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            struct pcie_prop p = { so + pos, len };
            if (pcie_prop_record(b, &current, b + no + nameoff, p) != 0) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            pos += len;
            uint32_t pad = (4U - (pos & 3U)) & 3U;
            if (!pcie_range_ok(ss, pos, pad)) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            pos += pad;
        } else if (token == 9U) {
            if (depth || !root || pos != ss) {
                pcie_zero(out, sizeof(*out));
                return -1;
            }
            end = true;
            break;
        } else if (token != 4U) {
            pcie_zero(out, sizeof(*out));
            return -1;
        }
    }

    if (!end) {
        pcie_zero(out, sizeof(*out));
        return -1;
    }
    return 0;
}
