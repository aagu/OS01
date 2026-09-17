/* Bounded, freestanding FDT v17 parser for the supported QEMU virt platform. */
#include <arch/aarch64/dtb.h>

#define FDT_MAGIC UINT32_C(0xd00dfeed)
#define MAX_DEPTH 32

struct property { const uint8_t *data; uint32_t len; };
struct node {
    const uint8_t *name;
    uint32_t address_cells, size_cells, parent_address_cells, parent_size_cells;
    bool cpus, cpu_child;
    struct property reg, compatible, device_type, status, method, enable, interrupts;
};
struct parse_state {
    struct aarch64_platform_info info;
    bool enable[AARCH64_BOOT_MAX_CPUS];
    bool psci, gic, uart, timer;
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
static bool strings_valid(struct property p)
{
    return p.len && p.data[p.len - 1] == 0;
}

static bool exact(struct property p, const char *s)
{
    uint32_t n = 0;
    while (s[n]) ++n;
    return p.data && p.len == n + 1 && equal(p.data, s);
}

static bool contains(struct property p, const char *s)
{
    uint32_t i = 0;
    while (i < p.len) {
        if (equal(p.data + i, s)) return true;
        while (p.data[i++]) { }
    }
    return false;
}

static uint64_t decode(const uint8_t *p, uint32_t count)
{
    return count == 2 ? (uint64_t)be32(p) << 32 | be32(p + 4) : be32(p);
}

static bool device_reg(const struct node *n, uint32_t index, uint64_t base)
{
    uint32_t ac = n->parent_address_cells, sc = n->parent_size_cells;
    if ((ac != 1 && ac != 2) || (sc != 1 && sc != 2)) return false;
    uint32_t stride = (ac + sc) * 4;
    if (!n->reg.data || n->reg.len % stride ||
        !range(n->reg.len, index * stride, stride)) return false;
    const uint8_t *p = n->reg.data + index * stride;
    return decode(p, ac) == base && decode(p + ac * 4, sc) >= 0x1000;
}

static int finish_node(struct parse_state *s, const struct node *n, uint32_t depth)
{
    struct aarch64_topology *t = &s->info.topology;
    if (n->cpu_child && exact(n->device_type, "cpu") &&
        (!n->status.data || exact(n->status, "okay") || exact(n->status, "ok"))) {
        uint32_t ac = n->parent_address_cells;
        if ((ac != 1 && ac != 2) || !n->reg.data || n->reg.len != ac * 4 ||
            t->cpu_count == AARCH64_BOOT_MAX_CPUS) return -2;
        uint64_t id = decode(n->reg.data, ac) & AARCH64_MPIDR_AFFINITY_MASK;
        for (uint32_t i = 0; i < t->cpu_count; ++i)
            if (t->mpidr[i] == id) return -2;
        s->enable[t->cpu_count] = exact(n->enable, "psci");
        t->mpidr[t->cpu_count++] = id;
    }
    /* Supported devices are root children: no untranslated nested buses. */
    if (depth != 2) return 0;
    if (node_is(n->name, "psci") || contains(n->compatible, "arm,psci-0.2") ||
        contains(n->compatible, "arm,psci-1.0") || contains(n->compatible, "arm,psci")) {
        if (s->psci) return -3;
        s->psci = true;
        t->psci_compatible = contains(n->compatible, "arm,psci-0.2") ||
                             contains(n->compatible, "arm,psci-1.0");
        if (!t->psci_compatible) return -3;
        if (exact(n->method, "hvc")) t->conduit = PSCI_CONDUIT_HVC;
        else if (exact(n->method, "smc")) t->conduit = PSCI_CONDUIT_SMC;
        else return -3;
    }
    if (contains(n->compatible, "arm,cortex-a15-gic")) {
        if (s->gic || (n->status.data && !exact(n->status, "okay") && !exact(n->status, "ok")) ||
            !device_reg(n, 0, 0x08000000) || !device_reg(n, 1, 0x08010000)) return -4;
        s->gic = true;
        s->info.gicd_base = 0x08000000;
        s->info.gicc_base = 0x08010000;
    }
    if (contains(n->compatible, "arm,pl011")) {
        /* interrupts = <GIC_SPI(0) nr IRQ_TYPE_LEVEL_HIGH(4)>: 3 be32 words
         * QEMU virt pl011: <0x00 0x01 0x04> → SPI 33. The parse keeps
         * INTID = 32 + nr; validation rejects any shape that would force
         * the consumer to hardcode the wire (R6). */
        if (s->uart || (n->status.data && !exact(n->status, "okay") && !exact(n->status, "ok")) ||
            !device_reg(n, 0, 0x09000000) ||
            n->interrupts.len != 12 ||
            be32(n->interrupts.data) != 0 ||
            be32(n->interrupts.data + 4) == 0 ||
            be32(n->interrupts.data + 8) != 4) return -4;
        s->uart = true;
        s->info.pl011_base = 0x09000000;
        s->info.pl011_spi = 32u + be32(n->interrupts.data + 4);
    }
    if (contains(n->compatible, "arm,armv8-timer")) {
        if (s->timer || (n->status.data && !exact(n->status, "okay") && !exact(n->status, "ok")) ||
            n->interrupts.len < 24 || n->interrupts.len % 12 ||
            be32(n->interrupts.data + 12) != 1 ||
            be32(n->interrupts.data + 16) != 14) return -4;
        s->timer = true;
        s->info.cntp_ppi = 30;
    }
    return 0;
}

static int property(struct node *n, const uint8_t *name, struct property p)
{
    struct property *dst = 0;
    bool str = false;
    if (equal(name, "#address-cells") || equal(name, "#size-cells")) {
        if (p.len != 4) return -1;
        if (equal(name, "#address-cells")) n->address_cells = be32(p.data);
        else n->size_cells = be32(p.data);
        return 0;
    }
    if (equal(name, "reg")) dst = &n->reg;
    else if (equal(name, "interrupts")) dst = &n->interrupts;
    else if (equal(name, "compatible")) { dst = &n->compatible; str = true; }
    else if (equal(name, "device_type")) { dst = &n->device_type; str = true; }
    else if (equal(name, "status")) { dst = &n->status; str = true; }
    else if (equal(name, "method")) { dst = &n->method; str = true; }
    else if (equal(name, "enable-method")) { dst = &n->enable; str = true; }
    if (str && !strings_valid(p)) return -1;
    if (dst) {
        if (dst->data) return -1;
        *dst = p;
    }
    return 0;
}

int aarch64_dtb_parse(const void *blob, uint32_t size, uint64_t bsp,
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
     * first child and retain only ancestry cells. A full node per depth
     * exceeds the early BSP's 4 KiB stack. */
    struct { uint32_t address_cells, size_cells; bool cpus; } stack[MAX_DEPTH];
    struct node current;
    zero(&state, sizeof(state));
    uint32_t pos = 0, depth = 0;
    bool root = false, end = false, pending = false;
    const uint8_t *st = b + so;
    while (pos < ss) {
        if (!range(ss, pos, 4)) return -1;
        uint32_t token = be32(st + pos);
        pos += 4;
        if (token == 1) {
            if (depth == MAX_DEPTH || (!depth && root)) return -1;
            if (pending) {
                int rc = finish_node(&state, &current, depth);
                if (rc) return rc;
                stack[depth - 1].address_cells = current.address_cells;
                stack[depth - 1].size_cells = current.size_cells;
                stack[depth - 1].cpus = current.cpus;
            }
            uint32_t start = pos;
            while (pos < ss && st[pos]) ++pos;
            if (pos == ss) return -1;
            ++pos;
            uint32_t pad = (4 - (pos & 3)) & 3;
            if (!range(ss, pos, pad)) return -1;
            pos += pad;
            struct node *n = &current;
            zero(n, sizeof(*n));
            n->name = st + start;
            n->address_cells = 2; n->size_cells = 1;
            if (depth) {
                n->parent_address_cells = stack[depth - 1].address_cells;
                n->parent_size_cells = stack[depth - 1].size_cells;
                n->cpu_child = stack[depth - 1].cpus;
            } else {
                if (st[start]) return -1;
                root = true;
            }
            n->cpus = depth == 1 && equal(n->name, "cpus");
            ++depth;
            pending = true;
        } else if (token == 2) {
            if (!depth) return -1;
            if (pending) {
                int rc = finish_node(&state, &current, depth);
                if (rc) return rc;
                pending = false;
            }
            --depth;
        } else if (token == 3) {
            if (!depth || !pending || !range(ss, pos, 8)) return -1;
            uint32_t len = be32(st + pos), nameoff = be32(st + pos + 4);
            pos += 8;
            if (!range(ss, pos, len) || nameoff >= ns) return -1;
            uint32_t ni = nameoff;
            while (ni < ns && b[no + ni]) ++ni;
            if (ni == ns) return -1;
            struct property p = { st + pos, len };
            int rc = property(&current, b + no + nameoff, p);
            if (rc) return rc;
            pos += len;
            uint32_t pad = (4 - (pos & 3)) & 3;
            if (!range(ss, pos, pad)) return -1;
            pos += pad;
        } else if (token == 9) {
            if (depth || !root || pos != ss) return -1;
            end = true;
            break;
        } else if (token != 4) return -1;
    }
    if (!end) return -1;
    struct aarch64_topology *t = &state.info.topology;
    bsp &= AARCH64_MPIDR_AFFINITY_MASK;
    uint32_t bsp_index = t->cpu_count;
    for (uint32_t i = 0; i < t->cpu_count; ++i)
        if (t->mpidr[i] == bsp) bsp_index = i;
    if (bsp_index == t->cpu_count) return -2;
    for (uint32_t i = 0; i < t->cpu_count; ++i)
        if (i != bsp_index && !state.enable[i]) return -3;
    if (t->cpu_count > 1 && !state.psci) return -3;
    /* Stable ordering keeps the firmware's AP order and places BSP at zero. */
    for (uint32_t i = bsp_index; i > 0; --i) t->mpidr[i] = t->mpidr[i - 1];
    t->mpidr[0] = bsp;
    if (!state.gic || !state.uart || !state.timer) return -4;
    /* Byte copy avoids an implicit freestanding memcpy dependency. */
    for (uint32_t i = 0; i < sizeof(*out); ++i)
        ((volatile uint8_t *)out)[i] = ((uint8_t *)&state.info)[i];
    return 0;
}
