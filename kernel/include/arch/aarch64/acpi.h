#ifndef OS01_AARCH64_ACPI_H
#define OS01_AARCH64_ACPI_H

#include <stdbool.h>
#include <stdint.h>

#define AARCH64_ACPI_SPCR_IFACE_PL011     0x03u
#define AARCH64_ACPI_SPCR_IFACE_SBSA      0x0Eu
#define AARCH64_ACPI_SPCR_IFACE_QCOM_GENI 0x13u

#define AARCH64_ACPI_MAX_CPUS 64u

struct aarch64_acpi_info {
    uint64_t spcr_base;
    uint32_t spcr_gsiv;
    uint8_t  spcr_iface;
    uint8_t  gic_version;
    uint64_t gicd_base;
    uint64_t gicc_base;
    uint64_t gicr_base;
    uint32_t gicr_len;
    uint32_t cntp_ppi;
    uint32_t cntv_ppi;
    uint32_t cpu_count;
    uint64_t cpu_mpidr[AARCH64_ACPI_MAX_CPUS];
};

/* Parse ACPI 2.0 RSDP -> XSDT -> SPCR / MADT(APIC) / GTDT within [0, max_phys).
 * Freestanding & host-linkable. Returns 0 on success, -1 on invalid tables. */
int aarch64_acpi_parse(uint64_t rsdp_phys, uint64_t max_phys,
                       struct aarch64_acpi_info *out);

#endif /* OS01_AARCH64_ACPI_H */
