#include "loader.h"
#include <stddef.h>
#include <string.h>

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

int aarch64_acpi_pack_handoff(uint64_t rsdp_phys, void *dst_page,
                              uint32_t dst_size, uint64_t dst_phys,
                              struct aarch64_acpi_info *out)
{
    const uint8_t *rsdp;
    const uint8_t *xsdt;
    uint32_t xsdt_len;
    uint8_t *dst = (uint8_t *)dst_page;
    uint32_t cursor = 0;
    const uint8_t *spcr = NULL, *madt = NULL, *gtdt = NULL;
    uint32_t spcr_len = 0, madt_len = 0, gtdt_len = 0;

    if (!rsdp_phys || !dst_page || dst_size < 4096u || !out)
        return -1;
    if (aarch64_acpi_parse(rsdp_phys, 0u, out) != 0)
        return -1;

    rsdp = (const uint8_t *)(__UINTPTR_TYPE__)rsdp_phys;
    xsdt = (const uint8_t *)(__UINTPTR_TYPE__)rd64(rsdp + 24);
    xsdt_len = rd32(xsdt + 4);
    for (uint32_t off = 36u; off + 8u <= xsdt_len; off += 8u) {
        uint64_t tp = rd64(xsdt + off);
        if (!tp)
            continue;
        const uint8_t *t = (const uint8_t *)(__UINTPTR_TYPE__)tp;
        uint32_t tl = rd32(t + 4);
        if (memcmp(t, "SPCR", 4) == 0 && !spcr) { spcr = t; spcr_len = tl; }
        else if (memcmp(t, "APIC", 4) == 0 && !madt) { madt = t; madt_len = tl; }
        else if (memcmp(t, "GTDT", 4) == 0 && !gtdt) { gtdt = t; gtdt_len = tl; }
    }

    uint32_t n_tbl = (spcr ? 1u : 0u) + (madt ? 1u : 0u) + (gtdt ? 1u : 0u);
    uint32_t new_xsdt_len = 36u + n_tbl * 8u;
    uint32_t total = 64u + new_xsdt_len + spcr_len + madt_len + gtdt_len;
    if (total > dst_size)
        return -1;

    memset(dst, 0, dst_size);
    memcpy(dst, rsdp, 36u);
    cursor = 64u;

    uint64_t new_xsdt_phys = dst_phys + cursor;
    memcpy(dst + cursor, xsdt, 36u);
    memcpy(dst + cursor + 4, &new_xsdt_len, 4u);
    uint32_t xsdt_off = cursor;
    cursor += new_xsdt_len;

    memcpy(dst + 24, &new_xsdt_phys, 8u);

    uint32_t idx = 0u;
    const uint8_t *src_tbls[3] = { spcr, madt, gtdt };
    uint32_t src_lens[3] = { spcr_len, madt_len, gtdt_len };
    for (int i = 0; i < 3; i++) {
        if (!src_tbls[i])
            continue;
        uint64_t tphys = dst_phys + cursor;
        memcpy(dst + cursor, src_tbls[i], src_lens[i]);
        memcpy(dst + xsdt_off + 36u + idx * 8u, &tphys, 8u);
        cursor += src_lens[i];
        idx++;
    }
    return 0;
}
