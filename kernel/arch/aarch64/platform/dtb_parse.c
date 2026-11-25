/* Bounded, freestanding FDT v17 parser for QEMU virt and simple-bus SoCs:
 * BCM2712 hangs its PL011 and GIC-400 off a /soc bus whose ranges property
 * translates child addresses into the CPU physical view. */
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/fdt_address.h>

#define FDT_MAGIC UINT32_C(0xd00dfeed)
#define MAX_DEPTH 8
#define MAX_FIXED_CLOCKS 4
/* Smallest register block a device may declare: a PL011 must reach through
 * ICR (0x44), the last register pl011.c touches (BCM2712 declares 0x200,
 * QEMU virt 0x1000); a GICD/GICC frame starts at 4 KiB. */
#define PL011_REG_SPAN 0x48
#define GIC_REG_SPAN 0x1000

struct property { uint32_t off; uint32_t len; };
struct node {
    uint32_t name_off;
    uint8_t address_cells, size_cells, parent_address_cells, parent_size_cells;
    bool cpus, cpu_child, has_phandle;
    uint32_t phandle, clock_frequency;
    struct property reg, compatible, device_type, status, method, enable, interrupts, clocks;
    struct property ranges;
};
/* One entry per open ancestor, indexed depth-1: the cells its children
 * encode addresses with, and its `ranges` (blob offset + length; a missing
 * or empty property means the bus is transparent). */
struct bus {
    uint32_t ranges_off, ranges_len;
    uint8_t address_cells, size_cells;
    bool has_ranges;
    bool cpus;
};
struct fixed_clock_entry { uint32_t phandle, freq; };
struct parse_state {
    struct aarch64_platform_info *info;
    struct fixed_clock_entry clocks[MAX_FIXED_CLOCKS];
    uint32_t clock_count;
    uint32_t uart_clk_phandle;
    uint64_t target_console_pa;
    bool firmware_configured;
    uint32_t uart_matches;
    bool enable[AARCH64_BOOT_MAX_CPUS];
    bool psci, gic, uart, timer;
    bool uart_ok;    /* the chosen PL011 is enabled, not a disabled stand-in */
};

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static bool range(uint32_t size, uint32_t off, uint32_t len)
{
    return off <= size && len <= size - off;
}

static void zero(void *dst, uint32_t size)
{
    volatile uint8_t *p = dst;
    while (size--) *p++ = 0;
}

static bool equal(const uint8_t *a, const char *b)
{
    while (*a && *b && *a == (uint8_t)*b) { ++a; ++b; }
    return *a == (uint8_t)*b;
}

static bool node_is(const uint8_t *a, const char *b)
{
    while (*b && *a == (uint8_t)*b) { ++a; ++b; }
    return !*b && (!*a || *a == '@');
}

/* All callers validate NUL termination before using string helpers. */
static bool strings_valid(const uint8_t *b, struct property p)
{
    return p.len && b[p.off + p.len - 1] == 0;
}

static bool exact(const uint8_t *b, struct property p, const char *s)
{
    uint32_t n = 0;
    while (s[n]) ++n;
    return p.off && p.len == n + 1 && equal(b + p.off, s);
}

/* A node without `status`, or with it okay/ok, is enabled by the firmware. */
static bool enabled(const uint8_t *b, const struct node *n)
{
    return !n->status.off || exact(b, n->status, "okay") || exact(b, n->status, "ok");
}

static bool contains(const uint8_t *b, struct property p, const char *s)
{
    uint32_t i = 0;
    const uint8_t *d = b + p.off;
    if (!p.off) return false;
    while (i < p.len) {
        if (equal(d + i, s)) return true;
        while (d[i++]) { }
    }
    return false;
}

static uint64_t decode(const uint8_t *p, uint32_t count)
{
    return count == 2 ? (uint64_t)be32(p) << 32 | be32(p + 4) : be32(p);
}

static bool device_reg_base(const uint8_t *b, const struct node *n, uint32_t index,
                            uint64_t min_size, uint64_t *base_out, uint64_t *size_out)
{
    uint32_t ac = n->parent_address_cells, sc = n->parent_size_cells;
    if ((ac != 1 && ac != 2) || (sc != 1 && sc != 2)) return false;
    uint32_t stride = (ac + sc) * 4;
    if (!n->reg.off || n->reg.len % stride ||
        !range(n->reg.len, index * stride, stride)) return false;
    const uint8_t *p = b + n->reg.off + index * stride;
    uint64_t sz = decode(p + ac * 4, sc);
    if (sz < min_size) return false;
    *base_out = decode(p, ac);
    if (size_out) *size_out = sz;
    return true;
}

/* Translate a node's bus interval [addr, addr + size) to the CPU physical view
 * using the shared fdt_address.h single-step translator. */
static bool to_cpu_pa(const uint8_t *b, const struct bus *stack, uint32_t depth,
                      uint64_t size, uint64_t *addr)
{
    for (uint32_t d = depth - 1; d > 1; --d) {
        const struct bus *bus = &stack[d - 1];
        struct aarch64_fdt_bus fbus = {
            .ranges = bus->has_ranges ? (b + bus->ranges_off) : 0,
            .ranges_len = bus->ranges_len,
            .child_cells = bus->address_cells,
            .parent_cells = stack[d - 2].address_cells,
            .size_cells = bus->size_cells,
            .has_ranges = bus->has_ranges,
        };
        if (!aarch64_fdt_translate_reg_step(&fbus, size, addr))
            return false;
    }
    return true;
}

static int finish_node(struct parse_state *s, const struct node *n, uint32_t depth,
                       const uint8_t *b, const struct bus *stack)
{
    struct aarch64_topology *t = &s->info->topology;
    if (n->cpu_child && exact(b, n->device_type, "cpu") && enabled(b, n)) {
        uint32_t ac = n->parent_address_cells;
        if ((ac != 1 && ac != 2) || !n->reg.off || n->reg.len != ac * 4 ||
            t->cpu_count == AARCH64_BOOT_MAX_CPUS) return -2;
        uint64_t id = decode(b + n->reg.off, ac) & AARCH64_MPIDR_AFFINITY_MASK;
        for (uint32_t i = 0; i < t->cpu_count; ++i)
            if (t->mpidr[i] == id) return -2;
        s->enable[t->cpu_count] = exact(b, n->enable, "psci");
        t->mpidr[t->cpu_count++] = id;
    }
    if (n->has_phandle && n->clock_frequency > 0) {
        if (s->uart && s->uart_clk_phandle == n->phandle && !s->info->pl011_clock)
            s->info->pl011_clock = n->clock_frequency;
        if (s->clock_count < MAX_FIXED_CLOCKS) {
            s->clocks[s->clock_count].phandle = n->phandle;
            s->clocks[s->clock_count].freq = n->clock_frequency;
            s->clock_count++;
        }
    }
    /* psci and timer are root children on every supported platform. */
    if (depth == 2 &&
        (node_is(b + n->name_off, "psci") || contains(b, n->compatible, "arm,psci-0.2") ||
         contains(b, n->compatible, "arm,psci-1.0") || contains(b, n->compatible, "arm,psci"))) {
        if (s->psci) return -3;
        s->psci = true;
        t->psci_compatible = contains(b, n->compatible, "arm,psci-0.2") ||
                             contains(b, n->compatible, "arm,psci-1.0");
        if (!t->psci_compatible) return -3;
        if (exact(b, n->method, "hvc")) t->conduit = PSCI_CONDUIT_HVC;
        else if (exact(b, n->method, "smc")) t->conduit = PSCI_CONDUIT_SMC;
        else return -3;
    }
    /* Devices are searched on every bus: SoCs hang them off simple-bus
     * containers whose ranges translate child addresses to the CPU view. */
    if (contains(b, n->compatible, "arm,cortex-a15-gic") ||
        contains(b, n->compatible, "arm,gic-400")) {
        uint64_t gicd_base = 0, gicd_size = 0;
        uint64_t gicc_base = 0, gicc_size = 0;
        /* A disabled GIC is absent, not an error. */
        if (!enabled(b, n)) return 0;
        if (s->gic ||
            !device_reg_base(b, n, 0, GIC_REG_SPAN, &gicd_base, &gicd_size) || gicd_base == 0 ||
            !device_reg_base(b, n, 1, GIC_REG_SPAN, &gicc_base, &gicc_size) || gicc_base == 0) return -4;
        if (!to_cpu_pa(b, stack, depth, gicd_size, &gicd_base) ||
            !to_cpu_pa(b, stack, depth, gicc_size, &gicc_base)) return 0;
        s->gic = true;
        s->info->gicd_base = gicd_base;
        s->info->gicc_base = gicc_base;
    }
    if (contains(b, n->compatible, "arm,pl011")) {
        uint64_t base = 0, size = 0;
        bool on = enabled(b, n);
        if (s->firmware_configured)
            return 0;
        if (s->target_console_pa != 0) {
            if (!on)
                return 0;
            if (!device_reg_base(b, n, 0, PL011_REG_SPAN, &base, &size))
                return -4;
            if (!to_cpu_pa(b, stack, depth, size, &base))
                return 0;
            if (base != s->target_console_pa)
                return 0;
            s->uart_matches++;
            if (s->uart_matches > 1)
                return -4;
        } else {
            /* The console is the first enabled PL011 in tree order.  A disabled
             * one only stands in when nothing enabled shows up, so a firmware
             * that left uart0 status=disabled still gets diagnostics instead of
             * a mute kernel; it never displaces an enabled console. */
            if (s->uart && (!on || s->uart_ok)) return 0;
            if (!device_reg_base(b, n, 0, PL011_REG_SPAN, &base, &size)) return on ? -4 : 0;
            if (!to_cpu_pa(b, stack, depth, size, &base)) return 0;
            if (base == 0) return on ? -4 : 0;
        }
        s->uart = true;
        s->uart_ok = on;
        s->info->pl011_base = base;
        s->info->pl011_spi = 0;
        s->info->pl011_clock = 0;
        s->uart_clk_phandle = 0;
        /* interrupts is optional / board-shaped: when present as 3-cell
         * GIC_SPI(0) entries <0 nr flags> with nr < 988 (INTID 32..1019),
         * extract the first entry's SPI INTID; otherwise degrade
         * pl011_spi to 0 so console.c falls back to output-only rather
         * than failing boot. */
        if (n->interrupts.off && n->interrupts.len >= 12 &&
            (n->interrupts.len % 12) == 0 &&
            be32(b + n->interrupts.off) == 0 &&
            be32(b + n->interrupts.off + 4) < 988) {
            s->info->pl011_spi = 32u + be32(b + n->interrupts.off + 4);
        } else {
            s->info->pl011_spi = 0;
        }
        if (n->clock_frequency > 0) {
            s->info->pl011_clock = n->clock_frequency;
        } else if (n->clocks.off && n->clocks.len >= 4) {
            uint32_t ph = be32(b + n->clocks.off);
            for (uint32_t i = 0; i < s->clock_count; ++i) {
                if (s->clocks[i].phandle == ph) {
                    s->info->pl011_clock = s->clocks[i].freq;
                    break;
                }
            }
            if (!s->info->pl011_clock)
                s->uart_clk_phandle = ph;
        }
    }
    if (contains(b, n->compatible, "virtio,mmio")) {
        /* Optional transport slot: collect reg base + SPI instead of
         * validating against fixed QEMU constants (the consumer probes
         * MAGIC/VERSION/DEVICEID at runtime).  reg = <addr size> with the
         * parent's cells; the virtio-mmio window is 512 bytes (0x200).
         * interrupts, when present, is <GIC_SPI(0) nr LEVEL(4)>. */
        if (enabled(b, n)) {
            uint32_t ac = n->parent_address_cells, sc = n->parent_size_cells;
            if ((ac != 1 && ac != 2) || (sc != 1 && sc != 2) ||
                !n->reg.off || n->reg.len != (ac + sc) * 4)
                return -4;
            const uint8_t *rp = b + n->reg.off;
            uint64_t base = decode(rp, ac);
            uint64_t size = decode(rp + ac * 4, sc);
            if (base == 0 || size < 0x200) return -4;
            if (!to_cpu_pa(b, stack, depth, size, &base)) return 0;
            if (s->info->virtio_mmio_count < AARCH64_VIRTIO_MMIO_MAX) {
                struct aarch64_virtio_mmio_slot *slot =
                    &s->info->virtio_mmio[s->info->virtio_mmio_count++];
                slot->base = base;
                slot->spi = (n->interrupts.off && n->interrupts.len == 12 &&
                             be32(b + n->interrupts.off) == 0 &&
                             be32(b + n->interrupts.off + 8) == 4)
                                ? 32u + be32(b + n->interrupts.off + 4)
                                : 0;
            }
        }
    }
    if (depth == 2 && contains(b, n->compatible, "arm,armv8-timer")) {
        if (s->timer || !enabled(b, n) ||
            !n->interrupts.off || n->interrupts.len < 24 || n->interrupts.len % 12 ||
            be32(b + n->interrupts.off + 12) != 1 ||
            be32(b + n->interrupts.off + 16) != 14) return -4;
        s->timer = true;
        s->info->cntp_ppi = 30;
    }
    return 0;
}

static int property(const uint8_t *b, struct node *n, const uint8_t *name, struct property p)
{
    struct property *dst = 0;
    bool str = false;
    if (equal(name, "#address-cells") || equal(name, "#size-cells")) {
        if (p.len != 4) return -1;
        uint32_t v = be32(b + p.off);
        if (equal(name, "#address-cells")) n->address_cells = (uint8_t)v;
        else n->size_cells = (uint8_t)v;
        return 0;
    }
    if (equal(name, "phandle") || equal(name, "linux,phandle")) {
        if (p.len != 4) return -1;
        if (!n->has_phandle || equal(name, "phandle")) {
            n->phandle = be32(b + p.off);
            n->has_phandle = true;
        }
        return 0;
    }
    if (equal(name, "clock-frequency")) {
        if (p.len == 4) {
            n->clock_frequency = be32(b + p.off);
        } else if (p.len == 8) {
            uint64_t f = decode(b + p.off, 2);
            if (f <= UINT32_MAX)
                n->clock_frequency = (uint32_t)f;
        } else {
            return -1;
        }
        return 0;
    }
    if (equal(name, "reg")) dst = &n->reg;
    else if (equal(name, "interrupts")) dst = &n->interrupts;
    else if (equal(name, "clocks")) dst = &n->clocks;
    else if (equal(name, "ranges")) dst = &n->ranges;
    else if (equal(name, "compatible")) { dst = &n->compatible; str = true; }
    else if (equal(name, "device_type")) { dst = &n->device_type; str = true; }
    else if (equal(name, "status")) { dst = &n->status; str = true; }
    else if (equal(name, "method")) { dst = &n->method; str = true; }
    else if (equal(name, "enable-method")) { dst = &n->enable; str = true; }
    if (str && !strings_valid(b, p)) return -1;
    if (dst) {
        if (dst->off) return -1;
        *dst = p;
    }
    return 0;
}

int aarch64_dtb_parse_for_console(const void *blob, uint32_t size,
                                  uint64_t bsp, uint64_t console_pa,
                                  bool firmware_configured,
                                  struct aarch64_platform_info *out)
{
    if (out) zero(out, sizeof(*out));
    if (!blob || !out || size < 40) return -1;
    const uint8_t *b = blob;
    uint32_t total = be32(b + 4), so = be32(b + 8), no = be32(b + 12);
    uint32_t ro = be32(b + 16), ns = be32(b + 32), ss = be32(b + 36);
    if (be32(b) != FDT_MAGIC || total < 40 || total > size ||
        be32(b + 20) < 17 || be32(b + 24) > 17 ||
        so < 40 || no < 40 || ro < 40 || (so & 3) || (ro & 7) ||
        !range(total, so, ss) || !range(total, no, ns) ||
        !range(total, ro, 16)) return -1;
    if (so < no + ns && no < so + ss) return -1;
    /* The reservation map must terminate within the blob, outside both blocks. */
    uint32_t rp = ro;
    for (;;) {
        if (!range(total, rp, 16) || (rp < so + ss && so < rp + 16) ||
            (rp < no + ns && no < rp + 16)) return -1;
        if (!decode(b + rp, 2) && !decode(b + rp + 8, 2)) break;
        rp += 16;
    }

    struct parse_state state;
    /* Properties precede children in FDT, so finalize each node before its
     * first child and retain only ancestry cells and ranges. A full node
     * per depth exceeds the early BSP's 4 KiB stack. */
    struct bus stack[MAX_DEPTH];
    struct node current;
    zero(&state, sizeof(state));
    state.info = out;
    state.target_console_pa = console_pa;
    state.firmware_configured = firmware_configured;
    uint32_t pos = 0, depth = 0;
    bool root = false, end = false, pending = false;
    const uint8_t *st = b + so;
    while (pos < ss) {
        if (!range(ss, pos, 4)) { zero(out, sizeof(*out)); return -1; }
        uint32_t token = be32(st + pos);
        pos += 4;
        if (token == 1) {
            if (depth == MAX_DEPTH || (!depth && root)) { zero(out, sizeof(*out)); return -1; }
            if (pending) {
                int rc = finish_node(&state, &current, depth, b, stack);
                if (rc) { zero(out, sizeof(*out)); return rc; }
                stack[depth - 1].address_cells = current.address_cells;
                stack[depth - 1].size_cells = current.size_cells;
                stack[depth - 1].cpus = current.cpus;
                stack[depth - 1].has_ranges = current.ranges.off != 0;
                stack[depth - 1].ranges_len = current.ranges.len;
                stack[depth - 1].ranges_off = current.ranges.off;
            }
            uint32_t start = pos;
            while (pos < ss && st[pos]) ++pos;
            if (pos == ss) { zero(out, sizeof(*out)); return -1; }
            ++pos;
            uint32_t pad = (4 - (pos & 3)) & 3;
            if (!range(ss, pos, pad)) { zero(out, sizeof(*out)); return -1; }
            pos += pad;
            struct node *n = &current;
            zero(n, sizeof(*n));
            n->name_off = so + start;
            n->address_cells = 2; n->size_cells = 1;
            if (depth) {
                n->parent_address_cells = stack[depth - 1].address_cells;
                n->parent_size_cells = stack[depth - 1].size_cells;
                n->cpu_child = stack[depth - 1].cpus;
            } else {
                if (st[start]) { zero(out, sizeof(*out)); return -1; }
                root = true;
            }
            n->cpus = depth == 1 && equal(b + n->name_off, "cpus");
            ++depth;
            pending = true;
        } else if (token == 2) {
            if (!depth) { zero(out, sizeof(*out)); return -1; }
            if (pending) {
                int rc = finish_node(&state, &current, depth, b, stack);
                if (rc) { zero(out, sizeof(*out)); return rc; }
                pending = false;
            }
            --depth;
        } else if (token == 3) {
            if (!depth || !pending || !range(ss, pos, 8)) { zero(out, sizeof(*out)); return -1; }
            uint32_t len = be32(st + pos), nameoff = be32(st + pos + 4);
            pos += 8;
            if (!range(ss, pos, len) || nameoff >= ns) { zero(out, sizeof(*out)); return -1; }
            uint32_t ni = nameoff;
            while (ni < ns && b[no + ni]) ++ni;
            if (ni == ns) { zero(out, sizeof(*out)); return -1; }
            struct property p = { so + pos, len };
            int rc = property(b, &current, b + no + nameoff, p);
            if (rc) { zero(out, sizeof(*out)); return rc; }
            pos += len;
            uint32_t pad = (4 - (pos & 3)) & 3;
            if (!range(ss, pos, pad)) { zero(out, sizeof(*out)); return -1; }
            pos += pad;
        } else if (token == 9) {
            if (depth || !root || pos != ss) { zero(out, sizeof(*out)); return -1; }
            end = true;
            break;
        } else if (token != 4) { zero(out, sizeof(*out)); return -1; }
    }
    if (!end) { zero(out, sizeof(*out)); return -1; }
    struct aarch64_topology *t = &state.info->topology;
    bsp &= AARCH64_MPIDR_AFFINITY_MASK;
    uint32_t bsp_index = t->cpu_count;
    for (uint32_t i = 0; i < t->cpu_count; ++i)
        if (t->mpidr[i] == bsp) bsp_index = i;
    if (bsp_index == t->cpu_count) { zero(out, sizeof(*out)); return -2; }
    for (uint32_t i = 0; i < t->cpu_count; ++i)
        if (i != bsp_index && !state.enable[i]) { zero(out, sizeof(*out)); return -3; }
    if (t->cpu_count > 1 && !state.psci) { zero(out, sizeof(*out)); return -3; }
    /* Stable ordering keeps the firmware's AP order and places BSP at zero. */
    for (uint32_t i = bsp_index; i > 0; --i) t->mpidr[i] = t->mpidr[i - 1];
    t->mpidr[0] = bsp;
    if (!state.gic || !state.timer) { zero(out, sizeof(*out)); return -4; }
    if (state.firmware_configured) {
        state.info->pl011_base = console_pa;
        state.info->pl011_spi = 0;
        state.info->pl011_clock = 0;
    } else {
        if (!state.uart) { zero(out, sizeof(*out)); return -4; }
        if (console_pa != 0 && state.uart_matches != 1) { zero(out, sizeof(*out)); return -4; }
    }
    return 0;
}

int aarch64_dtb_parse(const void *blob, uint32_t size, uint64_t bsp,
                      struct aarch64_platform_info *out)
{
    return aarch64_dtb_parse_for_console(blob, size, bsp, 0, false, out);
}
