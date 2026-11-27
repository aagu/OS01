#include "../../../include/arch/aarch64/acpi.h"
#include <stddef.h>

#define ACPI_PTR(addr) ((const uint8_t *)(__UINTPTR_TYPE__)(addr))

static uint32_t acpi_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t acpi_rd64(const uint8_t *p)
{
    return (uint64_t)acpi_rd32(p) | ((uint64_t)acpi_rd32(p + 4) << 32);
}

static bool acpi_sig_eq(const uint8_t *p, const char *sig)
{
    return p[0] == (uint8_t)sig[0] && p[1] == (uint8_t)sig[1] &&
           p[2] == (uint8_t)sig[2] && p[3] == (uint8_t)sig[3];
}

static bool acpi_range_ok(uint64_t addr, uint64_t len, uint64_t max_phys)
{
    if (addr == 0u || len == 0u)
        return false;
    if (max_phys != 0u) {
        if (addr >= max_phys || len > max_phys - addr)
            return false;
    } else if (addr > UINT64_MAX - len) {
        return false;
    }
    return true;
}

static void parse_spcr(const uint8_t *t, uint32_t len,
                       struct aarch64_acpi_info *out)
{
    if (len < 64u)
        return;
    out->spcr_iface = t[36];
    out->spcr_base = acpi_rd64(t + 44);
    out->spcr_gsiv = acpi_rd32(t + 56);
}

static void parse_madt(const uint8_t *t, uint32_t len,
                       struct aarch64_acpi_info *out)
{
    uint32_t off = 44u;

    if (len < 44u)
        return;
    while (off + 2u <= len) {
        uint8_t type = t[off];
        uint8_t rlen = t[off + 1u];
        if (rlen < 2u || off + rlen > len)
            break;
        if (type == 0x0Bu && rlen >= 76u) {
            uint32_t flags = acpi_rd32(t + off + 12u);
            uint64_t gicc = acpi_rd64(t + off + 36u);
            uint64_t gicr = acpi_rd64(t + off + 60u);
            uint64_t mpidr = acpi_rd64(t + off + 68u);
            if ((flags & 1u) && out->cpu_count < AARCH64_ACPI_MAX_CPUS)
                out->cpu_mpidr[out->cpu_count++] = mpidr;
            if (out->gicc_base == 0u && gicc != 0u)
                out->gicc_base = gicc;
            if (out->gicr_base == 0u && gicr != 0u)
                out->gicr_base = gicr;
        } else if (type == 0x0Cu && rlen >= 24u) {
            out->gicd_base = acpi_rd64(t + off + 8u);
            out->gic_version = t[off + 20u];
        } else if (type == 0x0Eu && rlen >= 16u) {
            if (out->gicr_base == 0u)
                out->gicr_base = acpi_rd64(t + off + 4u);
            out->gicr_len = acpi_rd32(t + off + 12u);
        }
        off += rlen;
    }
}

static void parse_gtdt(const uint8_t *t, uint32_t len,
                       struct aarch64_acpi_info *out)
{
    if (len < 80u)
        return;
    out->cntp_ppi = acpi_rd32(t + 56u);
    out->cntv_ppi = acpi_rd32(t + 64u);
}

int aarch64_acpi_parse(uint64_t rsdp_phys, uint64_t max_phys,
                       struct aarch64_acpi_info *out)
{
    const uint8_t *rsdp;
    const uint8_t *xsdt;
    uint64_t xsdt_phys;
    uint32_t xsdt_len;

    if (!out)
        return -1;
    for (size_t i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;

    if (!acpi_range_ok(rsdp_phys, 36u, max_phys))
        return -1;
    rsdp = ACPI_PTR(rsdp_phys);
    if (!acpi_sig_eq(rsdp, "RSD ") || !acpi_sig_eq(rsdp + 4, "PTR ") ||
        rsdp[15] < 2u)
        return -1;

    xsdt_phys = acpi_rd64(rsdp + 24);
    if (!acpi_range_ok(xsdt_phys, 36u, max_phys))
        return -1;
    xsdt = ACPI_PTR(xsdt_phys);
    if (!acpi_sig_eq(xsdt, "XSDT"))
        return -1;
    xsdt_len = acpi_rd32(xsdt + 4);
    if (xsdt_len < 36u || !acpi_range_ok(xsdt_phys, xsdt_len, max_phys))
        return -1;

    for (uint32_t off = 36u; off + 8u <= xsdt_len; off += 8u) {
        uint64_t tp = acpi_rd64(xsdt + off);
        if (!acpi_range_ok(tp, 36u, max_phys))
            continue;
        const uint8_t *t = ACPI_PTR(tp);
        uint32_t tl = acpi_rd32(t + 4);
        if (tl < 36u || !acpi_range_ok(tp, tl, max_phys))
            continue;
        if (acpi_sig_eq(t, "SPCR"))
            parse_spcr(t, tl, out);
        else if (acpi_sig_eq(t, "APIC"))
            parse_madt(t, tl, out);
        else if (acpi_sig_eq(t, "GTDT"))
            parse_gtdt(t, tl, out);
    }

    return 0;
}
