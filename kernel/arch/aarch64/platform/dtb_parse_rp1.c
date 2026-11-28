/* kernel/arch/aarch64/platform/dtb_parse_rp1.c — Bounded FDT v17 parser for RPi5 RP1 xHCI & IRQ */
#include <arch/aarch64/rp1_xhci.h>
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/fdt_address.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FDT_MAGIC       UINT32_C(0xd00dfeed)
#define FDT_BEGIN_NODE  1U
#define FDT_END_NODE    2U
#define FDT_PROP        3U
#define FDT_NOP         4U
#define FDT_END         9U
#define MAX_DEPTH       8U

struct fdt_frame {
    uint64_t bus_cpu_base;
    uint32_t reg_off;
    uint32_t reg_len;
    uint8_t addr_cells;
    uint8_t size_cells;
    uint8_t is_bcm_pcie : 1;
    uint8_t is_rp1 : 1;
    uint8_t is_dwc3 : 1;
    uint8_t status_ok : 1;
    uint8_t status_seen : 1;
    uint8_t dr_host : 1;
    uint8_t has_reg : 1;
    uint8_t has_ranges : 1;
    uint8_t has_bus_offset : 1;
};

static bool rp1_str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool rp1_compat_has(const uint8_t *prop, uint32_t len, const char *needle)
{
    uint32_t i = 0U;
    while (i < len) {
        const char *s = (const char *)(prop + i);
        uint32_t slen = 0U;
        while (i + slen < len && s[slen] != '\0')
            slen++;
        if (i + slen >= len)
            break;
        if (rp1_str_eq(s, needle))
            return true;
        i += slen + 1U;
    }
    return false;
}

static bool rp1_read_cells(const uint8_t *p, uint32_t cells, uint64_t *out)
{
    if (cells == 0U || cells > 2U)
        return false;
    uint64_t v = 0ULL;
    for (uint32_t i = 0U; i < cells; i++)
        v = (v << 32) | (uint64_t)aarch64_fdt_be32(p + i * 4U);
    *out = v;
    return true;
}

static bool rp1_node_enabled(const struct fdt_frame *f)
{
    return !f->status_seen || f->status_ok;
}

int aarch64_dtb_parse_rp1_xhci(const void *blob, uint32_t size,
                               struct aarch64_rp1_xhci_desc *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!blob || size < 40U)
        return -EINVAL;

    const uint8_t *b = (const uint8_t *)blob;
    if (aarch64_fdt_be32(b + 0U) != FDT_MAGIC)
        return -EINVAL;
    uint32_t totalsize = aarch64_fdt_be32(b + 4U);
    uint32_t off_struct = aarch64_fdt_be32(b + 8U);
    uint32_t off_strings = aarch64_fdt_be32(b + 12U);
    uint32_t sz_strings = aarch64_fdt_be32(b + 32U);
    uint32_t sz_struct = aarch64_fdt_be32(b + 36U);
    if (totalsize > size || totalsize < 40U ||
        off_struct > totalsize || sz_struct > totalsize - off_struct ||
        off_strings > totalsize || sz_strings > totalsize - off_strings) {
        return -EINVAL;
    }

    struct fdt_frame stack[MAX_DEPTH];
    memset(stack, 0, sizeof(stack));
    int depth = -1;
    bool board_ok = false;
    bool saw_bcm_pcie = false;
    uint64_t rc_reg_base = 0ULL, rc_reg_size = 0ULL;
    uint64_t rp1_child_base = 0ULL;
    bool has_rp1_child_base = false;
    struct aarch64_rp1_xhci_slot slots[AARCH64_RP1_MAX_XHCI];
    memset(slots, 0, sizeof(slots));
    uint32_t slot_count = 0U;

    uint32_t pos = off_struct;
    uint32_t end = off_struct + sz_struct;
    while (pos + 4U <= end) {
        uint32_t tok = aarch64_fdt_be32(b + pos);
        pos += 4U;
        if (tok == FDT_NOP)
            continue;
        if (tok == FDT_END)
            break;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + pos);
            uint32_t nlen = 0U;
            while (pos + nlen < end && name[nlen] != '\0')
                nlen++;
            if (pos + nlen >= end)
                goto invalid;
            pos += ((nlen + 1U) + 3U) & ~3U;

            depth++;
            if (depth >= (int)MAX_DEPTH)
                goto invalid;
            memset(&stack[depth], 0, sizeof(stack[depth]));
            stack[depth].addr_cells = 2U;
            stack[depth].size_cells = 1U;
            if (depth > 0 && (name[0] == 'r' && name[1] == 'p' && name[2] == '1'))
                stack[depth].is_rp1 = stack[depth - 1].is_bcm_pcie;
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (depth < 0)
                goto invalid;
            struct fdt_frame *cur = &stack[depth];
            /* On exit of axi/bus node, propagate nothing; on exit of bcm_pcie/rp1/dwc3 validate. */
            if (depth == 0 && !board_ok) {
                memset(out, 0, sizeof(*out));
                return -ENODEV;
            }
            if (cur->is_bcm_pcie && rp1_node_enabled(cur)) {
                if (!cur->has_reg || depth == 0)
                    goto invalid;
                uint32_t pac = stack[depth - 1].addr_cells;
                uint32_t psc = stack[depth - 1].size_cells;
                if (cur->reg_len < (pac + psc) * 4U)
                    goto invalid;
                uint64_t rbase = 0ULL, rsize = 0ULL;
                if (!rp1_read_cells(b + cur->reg_off, pac, &rbase) ||
                    !rp1_read_cells(b + cur->reg_off + pac * 4U, psc, &rsize) ||
                    rsize == 0ULL) {
                    goto invalid;
                }
                for (int d = depth - 1; d >= 0; d--) {
                    if (stack[d].has_bus_offset)
                        rbase += stack[d].bus_cpu_base;
                }
                if (saw_bcm_pcie)
                    goto invalid;
                saw_bcm_pcie = true;
                rc_reg_base = rbase;
                rc_reg_size = rsize;
            } else if (cur->is_rp1 && rp1_node_enabled(cur)) {
                if (!cur->has_ranges || depth == 0)
                    goto invalid;
            } else if (cur->is_dwc3 && rp1_node_enabled(cur) && cur->dr_host) {
                if (depth == 0 || !stack[depth - 1].is_rp1) {
                    /* DWC3 under non-RP1/non-bcm2712 RC is ignored if board matches, or ENODEV */
                } else {
                    if (!cur->has_reg || !has_rp1_child_base)
                        goto invalid;
                    uint32_t pac = stack[depth - 1].addr_cells;
                    uint32_t psc = stack[depth - 1].size_cells;
                    if (cur->reg_len < (pac + psc) * 4U)
                        goto invalid;
                    uint64_t base = 0ULL, rsz = 0ULL;
                    if (!rp1_read_cells(b + cur->reg_off, pac, &base) ||
                        !rp1_read_cells(b + cur->reg_off + pac * 4U, psc, &rsz) ||
                        rsz == 0ULL || base < rp1_child_base) {
                        goto invalid;
                    }
                    uint64_t off = base - rp1_child_base;
                    for (uint32_t i = 0U; i < slot_count; i++) {
                        uint64_t a0 = slots[i].rp1_offset;
                        uint64_t a1 = a0 + slots[i].size;
                        uint64_t b0 = off;
                        uint64_t b1 = b0 + rsz;
                        if (a0 < b1 && b0 < a1)
                            goto invalid; /* duplicate or overlapping */
                    }
                    if (slot_count >= AARCH64_RP1_MAX_XHCI)
                        goto invalid;
                    slots[slot_count++] = (struct aarch64_rp1_xhci_slot){
                        .valid = true,
                        .rp1_offset = off,
                        .size = rsz,
                    };
                }
            }
            depth--;
            continue;
        }
        if (tok == FDT_PROP) {
            if (pos + 8U > end || depth < 0)
                goto invalid;
            uint32_t plen = aarch64_fdt_be32(b + pos);
            uint32_t soff = aarch64_fdt_be32(b + pos + 4U);
            pos += 8U;
            if (soff >= sz_strings || pos + plen > end)
                goto invalid;
            const char *pname = (const char *)(b + off_strings + soff);
            const uint8_t *pval = b + pos;
            pos += (plen + 3U) & ~3U;

            struct fdt_frame *cur = &stack[depth];
            if (rp1_str_eq(pname, "#address-cells") && plen == 4U) {
                cur->addr_cells = (uint8_t)aarch64_fdt_be32(pval);
            } else if (rp1_str_eq(pname, "#size-cells") && plen == 4U) {
                cur->size_cells = (uint8_t)aarch64_fdt_be32(pval);
            } else if (rp1_str_eq(pname, "compatible")) {
                if (depth == 0 && rp1_compat_has(pval, plen, "raspberrypi,5-model-b"))
                    board_ok = true;
                if (rp1_compat_has(pval, plen, "brcm,bcm2712-pcie"))
                    cur->is_bcm_pcie = true;
                if (rp1_compat_has(pval, plen, "snps,dwc3"))
                    cur->is_dwc3 = true;
            } else if (rp1_str_eq(pname, "status") && plen > 0U) {
                cur->status_seen = true;
                cur->status_ok = rp1_str_eq((const char *)pval, "okay") ||
                                 rp1_str_eq((const char *)pval, "ok");
            } else if (rp1_str_eq(pname, "dr_mode") && plen > 0U) {
                cur->dr_host = rp1_str_eq((const char *)pval, "host");
            } else if (rp1_str_eq(pname, "reg")) {
                cur->has_reg = true;
                cur->reg_off = (uint32_t)(pval - b);
                cur->reg_len = plen;
            } else if (rp1_str_eq(pname, "ranges")) {
                cur->has_ranges = true;
                if (cur->is_rp1 && depth > 0) {
                    uint32_t cac = cur->addr_cells;
                    uint32_t pac = stack[depth - 1].addr_cells;
                    uint32_t csc = cur->size_cells;
                    uint32_t entry_cells = cac + pac + csc;
                    if (pac != 3U || plen == 0U || (plen % (entry_cells * 4U)) != 0U)
                        goto invalid;
                    uint32_t phys_hi = aarch64_fdt_be32(pval + cac * 4U);
                    uint32_t space = (phys_hi >> 24) & 0x3U;
                    if (space != 2U && space != 3U)
                        goto invalid;
                    uint64_t cbase = 0ULL;
                    if (!rp1_read_cells(pval, cac, &cbase))
                        goto invalid;
                    rp1_child_base = cbase;
                    has_rp1_child_base = true;
                } else if (depth > 0 && !cur->is_bcm_pcie) {
                    uint32_t cac = cur->addr_cells;
                    uint32_t pac = stack[depth - 1].addr_cells;
                    uint32_t csc = cur->size_cells;
                    if (plen >= (cac + pac + csc) * 4U) {
                        uint64_t c = 0ULL, p = 0ULL;
                        if (rp1_read_cells(pval, cac, &c) &&
                            rp1_read_cells(pval + cac * 4U, pac, &p) && p >= c) {
                            cur->has_bus_offset = true;
                            cur->bus_cpu_base = p - c;
                        }
                    }
                }
            }
            continue;
        }
        goto invalid;
    }

    if (!board_ok || !saw_bcm_pcie || slot_count == 0U)
        return -ENODEV;

    out->valid = true;
    out->rc_reg_base = rc_reg_base;
    out->rc_reg_size = rc_reg_size;
    out->rp1_child_base = rp1_child_base;
    out->controller_count = slot_count;
    for (uint32_t i = 0U; i < slot_count; i++)
        out->controllers[i] = slots[i];
    return 0;

invalid:
    memset(out, 0, sizeof(*out));
    return -EINVAL;
}

/* ── Independent RP1 / BCM2712 MIP / UART0 IRQ topology parser ────────── */
struct irq_fdt_frame {
    uint32_t reg_off, reg_len;
    uint32_t ranges_off, ranges_len;
    uint32_t intr_off, intr_len;
    uint32_t phandle;
    uint32_t eff_intr_parent;
    uint32_t msi_parent;
    uint64_t msi_pci_addr;
    uint32_t msi_base_spi;
    uint32_t msi_num_spis;
    uint32_t msi_offset;
    uint64_t rp1_child_base;
    uint64_t rp1_parent_cpu_base;
    uint8_t addr_cells;
    uint8_t size_cells;
    uint16_t flags;
};

#define IRQ_F_STATUS_SEEN    (1U << 0)
#define IRQ_F_STATUS_OK      (1U << 1)
#define IRQ_F_HAS_REG        (1U << 2)
#define IRQ_F_HAS_RANGES     (1U << 3)
#define IRQ_F_HAS_INTR_PAR   (1U << 5)
#define IRQ_F_HAS_MSI_PAR    (1U << 6)
#define IRQ_F_HAS_MSI_ADDR   (1U << 7)
#define IRQ_F_HAS_MSI_BASE   (1U << 8)
#define IRQ_F_HAS_MSI_COUNT  (1U << 9)
#define IRQ_F_IS_GIC         (1U << 10)
#define IRQ_F_IS_MIP         (1U << 11)
#define IRQ_F_IS_PCIE        (1U << 12)
#define IRQ_F_IS_RP1         (1U << 13)
#define IRQ_F_IS_UART        (1U << 14)
#define IRQ_F_HAS_INTR       (1U << 15)

struct irq_mip_cand {
    bool valid;
    uint32_t phandle;
    uint32_t intr_parent;
    uint64_t cpu_pa;
    uint64_t span;
    uint64_t msi_pci_addr;
    uint32_t base_spi;
    uint32_t count;
    uint32_t offset;
};

static struct {
    struct irq_fdt_frame stack[MAX_DEPTH];
    struct irq_mip_cand mips[4];
} s_irq_parse_scratch;

static bool irq_translate_one_ranges(const uint8_t *b,
                                     const struct irq_fdt_frame *bus,
                                     uint32_t pac, uint64_t *addr_io)
{
    if ((bus->flags & IRQ_F_HAS_RANGES) == 0U || bus->ranges_len == 0U)
        return true;
    uint32_t cac = bus->addr_cells;
    uint32_t csc = bus->size_cells;
    uint32_t stride = (cac + pac + csc) * 4U;
    if (stride == 0U || (bus->ranges_len % stride) != 0U)
        return false;
    const uint8_t *rp = b + bus->ranges_off;
    uint32_t n = bus->ranges_len / stride;
    uint64_t addr = *addr_io;
    for (uint32_t i = 0U; i < n; i++) {
        const uint8_t *e = rp + i * stride;
        uint64_t c = 0ULL, p = 0ULL, sz = 0ULL;
        uint32_t c_skip = (cac == 3U) ? 1U : 0U;
        if (!rp1_read_cells(e + c_skip * 4U, cac - c_skip, &c) ||
            !rp1_read_cells(e + cac * 4U, pac, &p) ||
            !rp1_read_cells(e + (cac + pac) * 4U, csc, &sz))
            continue;
        if (sz > 0ULL && addr >= c && addr - c < sz) {
            *addr_io = p + (addr - c);
            return true;
        }
    }
    return false;
}

static uint64_t irq_translate_bus(const uint8_t *b,
                                  const struct irq_fdt_frame *stack,
                                  int depth, uint64_t addr)
{
    for (int d = depth - 1; d >= 1; d--) {
        if ((stack[d].flags & IRQ_F_IS_PCIE) != 0U)
            continue;
        uint32_t pac = stack[d - 1].addr_cells;
        (void)irq_translate_one_ranges(b, &stack[d], pac, &addr);
    }
    return addr;
}

static bool irq_frame_enabled(const struct irq_fdt_frame *f)
{
    return (f->flags & IRQ_F_STATUS_SEEN) == 0U ||
           (f->flags & IRQ_F_STATUS_OK) != 0U;
}

int aarch64_dtb_parse_rp1_irq(const void *blob, uint32_t size,
                              uint64_t selected_gicd, uint64_t console_pa,
                              struct aarch64_rp1_irq_desc *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!blob || size < 40U || selected_gicd == 0ULL)
        return -EINVAL;

    const uint8_t *b = (const uint8_t *)blob;
    if (aarch64_fdt_be32(b + 0U) != FDT_MAGIC)
        return -EINVAL;
    uint32_t totalsize = aarch64_fdt_be32(b + 4U);
    uint32_t off_struct = aarch64_fdt_be32(b + 8U);
    uint32_t off_strings = aarch64_fdt_be32(b + 12U);
    uint32_t sz_strings = aarch64_fdt_be32(b + 32U);
    uint32_t sz_struct = aarch64_fdt_be32(b + 36U);
    if (totalsize > size || totalsize < 40U ||
        off_struct > totalsize || sz_struct > totalsize - off_struct ||
        off_strings > totalsize || sz_strings > totalsize - off_strings)
        return -EINVAL;

    memset(&s_irq_parse_scratch, 0, sizeof(s_irq_parse_scratch));
    struct irq_fdt_frame *stack = s_irq_parse_scratch.stack;
    struct irq_mip_cand *mips = s_irq_parse_scratch.mips;
    int depth = -1;
    bool board_ok = false;
    uint32_t gic_phandle = 0U;
    uint32_t mip_count = 0U;

    bool have_rp1 = false;
    bool ambiguous_rp1 = false;
    uint32_t sel_msi_parent = 0U;
    uint64_t sel_rc_base = 0ULL, sel_rc_size = 0ULL;
    uint64_t sel_rp1_child_base = 0ULL;
    bool sel_uart_valid = false;
    uint32_t sel_uart_source = 0U;
    uint64_t sel_uart_offset = 0ULL;

    bool cur_rp1_uart_valid = false;
    uint32_t cur_rp1_uart_source = 0U;
    uint64_t cur_rp1_uart_offset = 0ULL;

    uint32_t pos = off_struct;
    uint32_t end = off_struct + sz_struct;
    while (pos + 4U <= end) {
        uint32_t tok = aarch64_fdt_be32(b + pos);
        pos += 4U;
        if (tok == FDT_NOP)
            continue;
        if (tok == FDT_END)
            break;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + pos);
            uint32_t nlen = 0U;
            while (pos + nlen < end && name[nlen] != '\0')
                nlen++;
            if (pos + nlen >= end)
                goto invalid;
            pos += ((nlen + 1U) + 3U) & ~3U;

            depth++;
            if (depth >= (int)MAX_DEPTH)
                goto invalid;
            memset(&stack[depth], 0, sizeof(stack[depth]));
            stack[depth].addr_cells = 2U;
            stack[depth].size_cells = 1U;
            if (depth > 0) {
                struct irq_fdt_frame *par = &stack[depth - 1];
                if ((par->flags & IRQ_F_HAS_RANGES) != 0U &&
                    (par->flags & IRQ_F_IS_RP1) != 0U && depth >= 2) {
                    uint32_t cac = par->addr_cells;
                    uint32_t pac = stack[depth - 2].addr_cells;
                    uint32_t csc = par->size_cells;
                    uint32_t entry_cells = cac + pac + csc;
                    if (pac == 3U && par->ranges_len >= entry_cells * 4U &&
                        (par->ranges_len % (entry_cells * 4U)) == 0U) {
                        const uint8_t *rp = b + par->ranges_off;
                        uint64_t cbase = 0ULL, pbase = 0ULL;
                        if (rp1_read_cells(rp, cac, &cbase) &&
                            rp1_read_cells(rp + (cac + 1U) * 4U, 2U, &pbase)) {
                            par->rp1_child_base = cbase;
                            /* Translate PCI address through parent PCIe RC ranges if present. */
                            if (depth >= 3 &&
                                (stack[depth - 2].flags & IRQ_F_IS_PCIE) != 0U &&
                                (stack[depth - 2].flags & IRQ_F_HAS_RANGES) != 0U) {
                                (void)irq_translate_one_ranges(
                                    b, &stack[depth - 2],
                                    stack[depth - 3].addr_cells, &pbase);
                            }
                            par->rp1_parent_cpu_base = pbase;
                        }
                    }
                }
                stack[depth].eff_intr_parent = stack[depth - 1].eff_intr_parent;
                if ((stack[depth - 1].flags & IRQ_F_IS_PCIE) != 0U &&
                    name[0] == 'r' && name[1] == 'p' && name[2] == '1') {
                    stack[depth].flags |= IRQ_F_IS_RP1;
                    cur_rp1_uart_valid = false;
                    cur_rp1_uart_source = 0U;
                    cur_rp1_uart_offset = 0ULL;
                }
            }
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (depth < 0)
                goto invalid;
            struct irq_fdt_frame *cur = &stack[depth];

            if ((cur->flags & IRQ_F_IS_GIC) != 0U && irq_frame_enabled(cur) &&
                (cur->flags & IRQ_F_HAS_REG) != 0U && depth > 0 && cur->phandle != 0U) {
                uint32_t pac = stack[depth - 1].addr_cells;
                uint32_t psc = stack[depth - 1].size_cells;
                uint64_t gbase = 0ULL, gsize = 0ULL;
                if (cur->reg_len >= (pac + psc) * 4U &&
                    rp1_read_cells(b + cur->reg_off, pac, &gbase) &&
                    rp1_read_cells(b + cur->reg_off + pac * 4U, psc, &gsize) &&
                    gsize > 0ULL) {
                    if (irq_translate_bus(b, stack, depth, gbase) == selected_gicd)
                        gic_phandle = cur->phandle;
                }
            } else if ((cur->flags & IRQ_F_IS_MIP) != 0U && irq_frame_enabled(cur) &&
                       (cur->flags & IRQ_F_HAS_REG) != 0U && depth > 0 &&
                       cur->phandle != 0U &&
                       (cur->flags & IRQ_F_HAS_MSI_ADDR) != 0U &&
                       (cur->flags & IRQ_F_HAS_MSI_BASE) != 0U &&
                       (cur->flags & IRQ_F_HAS_MSI_COUNT) != 0U) {
                uint32_t pac = stack[depth - 1].addr_cells;
                uint32_t psc = stack[depth - 1].size_cells;
                uint64_t mbase = 0ULL, msize = 0ULL;
                if (cur->reg_len >= (pac + psc) * 4U &&
                    rp1_read_cells(b + cur->reg_off, pac, &mbase) &&
                    rp1_read_cells(b + cur->reg_off + pac * 4U, psc, &msize) &&
                    msize >= 0xc0ULL && mip_count < 4U) {
                    mips[mip_count++] = (struct irq_mip_cand){
                        .valid = true,
                        .phandle = cur->phandle,
                        .intr_parent = cur->eff_intr_parent,
                        .cpu_pa = irq_translate_bus(b, stack, depth, mbase),
                        .span = msize,
                        .msi_pci_addr = cur->msi_pci_addr,
                        .base_spi = cur->msi_base_spi,
                        .count = cur->msi_num_spis,
                        .offset = cur->msi_offset,
                    };
                }
            } else if ((cur->flags & IRQ_F_IS_UART) != 0U && depth > 0 &&
                       (stack[depth - 1].flags & IRQ_F_IS_RP1) != 0U) {
                const struct irq_fdt_frame *rp1 = &stack[depth - 1];
                if ((cur->flags & IRQ_F_HAS_REG) != 0U &&
                    (cur->flags & IRQ_F_HAS_INTR) != 0U &&
                    (rp1->flags & IRQ_F_HAS_RANGES) != 0U &&
                    rp1->phandle != 0U &&
                    cur->eff_intr_parent == rp1->phandle &&
                    cur->intr_len == 8U) {
                    uint32_t pac = rp1->addr_cells;
                    uint32_t psc = rp1->size_cells;
                    uint64_t ubase = 0ULL, usize = 0ULL;
                    if (cur->reg_len >= (pac + psc) * 4U &&
                        rp1_read_cells(b + cur->reg_off, pac, &ubase) &&
                        rp1_read_cells(b + cur->reg_off + pac * 4U, psc, &usize) &&
                        usize > 0ULL && ubase >= rp1->rp1_child_base) {
                        uint64_t uoff = ubase - rp1->rp1_child_base;
                        uint64_t cpu_pa = rp1->rp1_parent_cpu_base + uoff;
                        uint32_t src = aarch64_fdt_be32(b + cur->intr_off);
                        uint32_t typ = aarch64_fdt_be32(b + cur->intr_off + 4U);
                        if (typ == 4U && src < AARCH64_RP1_NUM_IRQS &&
                            console_pa != 0ULL && cpu_pa == console_pa) {
                            cur_rp1_uart_valid = true;
                            cur_rp1_uart_source = src;
                            cur_rp1_uart_offset = uoff;
                        }
                    }
                }
            } else if ((cur->flags & IRQ_F_IS_RP1) != 0U && irq_frame_enabled(cur) &&
                       depth >= 2 && (stack[depth - 1].flags & IRQ_F_IS_PCIE) != 0U &&
                       irq_frame_enabled(&stack[depth - 1])) {
                const struct irq_fdt_frame *rc = &stack[depth - 1];
                if ((cur->flags & IRQ_F_HAS_RANGES) != 0U &&
                    (rc->flags & IRQ_F_HAS_REG) != 0U &&
                    (rc->flags & IRQ_F_HAS_MSI_PAR) != 0U) {
                    uint32_t pac = stack[depth - 2].addr_cells;
                    uint32_t psc = stack[depth - 2].size_cells;
                    uint64_t rbase = 0ULL, rsize = 0ULL;
                    if (rc->reg_len >= (pac + psc) * 4U &&
                        rp1_read_cells(b + rc->reg_off, pac, &rbase) &&
                        rp1_read_cells(b + rc->reg_off + pac * 4U, psc, &rsize) &&
                        rsize > 0ULL) {
                        if (have_rp1) {
                            ambiguous_rp1 = true;
                        } else {
                            have_rp1 = true;
                            sel_msi_parent = rc->msi_parent;
                            sel_rc_base = irq_translate_bus(b, stack, depth - 1, rbase);
                            sel_rc_size = rsize;
                            sel_rp1_child_base = cur->rp1_child_base;
                            sel_uart_valid = cur_rp1_uart_valid;
                            sel_uart_source = cur_rp1_uart_source;
                            sel_uart_offset = cur_rp1_uart_offset;
                        }
                    }
                }
            }
            depth--;
            continue;
        }
        if (tok == FDT_PROP) {
            if (pos + 8U > end || depth < 0)
                goto invalid;
            uint32_t plen = aarch64_fdt_be32(b + pos);
            uint32_t soff = aarch64_fdt_be32(b + pos + 4U);
            pos += 8U;
            if (soff >= sz_strings || pos + plen > end)
                goto invalid;
            const char *pname = (const char *)(b + off_strings + soff);
            const uint8_t *pval = b + pos;
            pos += (plen + 3U) & ~3U;

            struct irq_fdt_frame *cur = &stack[depth];
            if (rp1_str_eq(pname, "#address-cells") && plen == 4U) {
                cur->addr_cells = (uint8_t)aarch64_fdt_be32(pval);
            } else if (rp1_str_eq(pname, "#size-cells") && plen == 4U) {
                cur->size_cells = (uint8_t)aarch64_fdt_be32(pval);
            } else if (rp1_str_eq(pname, "compatible")) {
                if (depth == 0 && rp1_compat_has(pval, plen, "raspberrypi,5-model-b"))
                    board_ok = true;
                if (rp1_compat_has(pval, plen, "arm,gic-400") ||
                    rp1_compat_has(pval, plen, "arm,cortex-a15-gic") ||
                    rp1_compat_has(pval, plen, "arm,gic-v2"))
                    cur->flags |= IRQ_F_IS_GIC;
                if (rp1_compat_has(pval, plen, "brcm,bcm2712-mip-intc"))
                    cur->flags |= IRQ_F_IS_MIP;
                if (rp1_compat_has(pval, plen, "brcm,bcm2712-pcie"))
                    cur->flags |= IRQ_F_IS_PCIE;
                if (rp1_compat_has(pval, plen, "arm,pl011") ||
                    rp1_compat_has(pval, plen, "arm,pl011-axi"))
                    cur->flags |= IRQ_F_IS_UART;
            } else if (rp1_str_eq(pname, "status") && plen > 0U) {
                cur->flags |= IRQ_F_STATUS_SEEN;
                if (rp1_str_eq((const char *)pval, "okay") ||
                    rp1_str_eq((const char *)pval, "ok"))
                    cur->flags |= IRQ_F_STATUS_OK;
            } else if ((rp1_str_eq(pname, "phandle") || rp1_str_eq(pname, "linux,phandle")) &&
                       plen == 4U) {
                cur->phandle = aarch64_fdt_be32(pval);
            } else if (rp1_str_eq(pname, "interrupt-parent") && plen == 4U) {
                cur->eff_intr_parent = aarch64_fdt_be32(pval);
                cur->flags |= IRQ_F_HAS_INTR_PAR;
            } else if (rp1_str_eq(pname, "msi-parent") && plen == 4U) {
                cur->msi_parent = aarch64_fdt_be32(pval);
                cur->flags |= IRQ_F_HAS_MSI_PAR;
            } else if (rp1_str_eq(pname, "reg")) {
                cur->reg_off = (uint32_t)(pval - b);
                cur->reg_len = plen;
                cur->flags |= IRQ_F_HAS_REG;
            } else if (rp1_str_eq(pname, "interrupts")) {
                cur->intr_off = (uint32_t)(pval - b);
                cur->intr_len = plen;
                cur->flags |= IRQ_F_HAS_INTR;
            } else if (rp1_str_eq(pname, "brcm,msi-pci-addr") && plen == 8U) {
                cur->msi_pci_addr = ((uint64_t)aarch64_fdt_be32(pval) << 32) |
                                    (uint64_t)aarch64_fdt_be32(pval + 4U);
                cur->flags |= IRQ_F_HAS_MSI_ADDR;
            } else if (rp1_str_eq(pname, "brcm,msi-base-spi") && plen == 4U) {
                cur->msi_base_spi = aarch64_fdt_be32(pval);
                cur->flags |= IRQ_F_HAS_MSI_BASE;
            } else if (rp1_str_eq(pname, "brcm,msi-num-spis") && plen == 4U) {
                cur->msi_num_spis = aarch64_fdt_be32(pval);
                cur->flags |= IRQ_F_HAS_MSI_COUNT;
            } else if (rp1_str_eq(pname, "brcm,msi-offset") && plen == 4U) {
                cur->msi_offset = aarch64_fdt_be32(pval);
            } else if (rp1_str_eq(pname, "ranges")) {
                cur->ranges_off = (uint32_t)(pval - b);
                cur->ranges_len = plen;
                cur->flags |= IRQ_F_HAS_RANGES;
            }
            continue;
        }
        goto invalid;
    }

    if (!board_ok || !have_rp1 || ambiguous_rp1 || gic_phandle == 0U)
        return -ENODEV;

    const struct irq_mip_cand *matched_mip = NULL;
    for (uint32_t i = 0U; i < mip_count; i++) {
        if (mips[i].valid && mips[i].phandle == sel_msi_parent) {
            if (matched_mip != NULL)
                return -EINVAL;
            matched_mip = &mips[i];
        }
    }
    if (!matched_mip || matched_mip->intr_parent != gic_phandle)
        return -ENODEV;
    if (matched_mip->count == 0U || matched_mip->offset >= 64U ||
        matched_mip->count > 64U - matched_mip->offset)
        return -EINVAL;
    if ((uint64_t)matched_mip->base_spi + matched_mip->offset + matched_mip->count > 1020ULL - 32ULL)
        return -EINVAL;

    out->rc_reg_base = sel_rc_base;
    out->rc_reg_size = sel_rc_size;
    out->rp1_child_base = sel_rp1_child_base;
    out->mip_cpu_pa = matched_mip->cpu_pa;
    out->mip_span = matched_mip->span;
    out->msi_pci_addr = matched_mip->msi_pci_addr;
    out->base_spi = matched_mip->base_spi;
    out->count = matched_mip->count;
    out->offset = matched_mip->offset;
    out->uart_valid = sel_uart_valid;
    out->uart_source = sel_uart_source;
    out->uart_offset = sel_uart_offset;
    out->valid = true;
    return 0;

invalid:
    memset(out, 0, sizeof(*out));
    return -EINVAL;
}
