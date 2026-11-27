/* kernel/arch/aarch64/platform/dtb_parse_rp1.c — Bounded FDT v17 parser for RPi5 RP1 xHCI */
#include <arch/aarch64/rp1_xhci.h>
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
