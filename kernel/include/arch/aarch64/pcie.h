#ifndef OS01_AARCH64_PCIE_H
#define OS01_AARCH64_PCIE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct boot_context;

#define AARCH64_PCIE_MAX_ROOTS   4U
#define AARCH64_PCIE_MAX_RANGES  8U
#define AARCH64_PCIE_MAX_INTMAP  32U

enum aarch64_pcie_ctrl_kind {
    AARCH64_PCIE_CTRL_NONE = 0,
    AARCH64_PCIE_CTRL_ECAM_GENERIC = 1,
    AARCH64_PCIE_CTRL_BCM2712 = 2, /* Phase-2 RPi5 reservation */
};

struct aarch64_pcie_range {
    uint8_t space;        /* PCI_SPACE_IO / PCI_SPACE_MMIO32 / PCI_SPACE_MMIO64 */
    bool prefetchable;
    bool mapped;          /* true iff Stage-3 pre-mapped this window as Device-nGnRnE */
    uint64_t pci_base;
    uint64_t cpu_base;
    uint64_t size;
};

struct aarch64_pcie_intmap_entry {
    uint32_t addr_hi;
    uint32_t int_pin;
    uint32_t gic_intid;
};

struct aarch64_pcie_v2m_desc {
    bool valid;
    uint32_t phandle;
    uint64_t reg_cpu_base;
    uint64_t reg_size;
    uint32_t msi_base_spi;
    uint32_t msi_num_spis;
};

struct aarch64_pcie_root_desc {
    bool valid;
    enum aarch64_pcie_ctrl_kind kind;
    uint16_t domain;
    uint8_t bus_start;
    uint8_t bus_end;
    uint32_t msi_phandle;
    uint64_t reg_cpu_base;
    uint64_t reg_size;
    uint8_t range_count;
    struct aarch64_pcie_range ranges[AARCH64_PCIE_MAX_RANGES];
    bool has_int_map;
    uint8_t int_map_count;
    uint32_t int_map_mask[4];
    struct aarch64_pcie_intmap_entry int_map[AARCH64_PCIE_MAX_INTMAP];
};

struct aarch64_pcie_topology {
    uint32_t root_count;
    struct aarch64_pcie_root_desc roots[AARCH64_PCIE_MAX_ROOTS];
    struct aarch64_pcie_v2m_desc v2m;
};

int aarch64_dtb_parse_pcie(const void *blob, uint32_t size,
                           struct aarch64_pcie_topology *out);
const struct aarch64_pcie_topology *aarch64_pcie_topology_get(void);
int aarch64_pcie_prepare(const struct boot_context *ctx);

#endif /* OS01_AARCH64_PCIE_H */
