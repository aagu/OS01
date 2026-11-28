#ifndef ARCH_AARCH64_RP1_PCIE_H
#define ARCH_AARCH64_RP1_PCIE_H

#include <arch/aarch64/rp1_xhci.h>
#include <core/bootinfo.h>
#include <stdbool.h>
#include <stdint.h>

#define AARCH64_RP1_NUM_IRQS     61U
#define AARCH64_RP1_APB_OFFSET   0x108000ULL
#define AARCH64_RP1_APB_SPAN     0x1000ULL

struct aarch64_rp1_irq_desc {
    uint64_t rc_reg_base;
    uint64_t rc_reg_size;
    uint64_t rp1_child_base;
    uint64_t mip_cpu_pa;
    uint64_t mip_span;
    uint64_t msi_pci_addr;
    uint64_t uart_offset;
    uint32_t base_spi;
    uint32_t count;
    uint32_t offset;
    uint32_t uart_source;
    bool valid;
    bool uart_valid;
};

struct aarch64_rp1_endpoint {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t revision;
    uint64_t bar0_pci;
    uint64_t bar0_cpu;
    uint64_t bar1_pci;
    uint64_t bar1_cpu;
    uint64_t outbound_pci_base;
    uint64_t outbound_cpu_base;
    uint64_t outbound_size;
};

enum aarch64_rp1_mmio_space {
    RP1_MMIO_RC = 0,
    RP1_MMIO_MIP = 1,
    RP1_MMIO_APB = 2,
    RP1_MMIO_TABLE = 3,
    RP1_MMIO_PBA = 4,
};

struct aarch64_rp1_irq_platform {
    bool valid;
    struct aarch64_rp1_irq_desc desc;
    struct aarch64_rp1_endpoint endpoint;
    struct boot_rp1_pci_info bar0;
    uint32_t endpoint_bdf;
    volatile uint8_t *rc_va;
    volatile uint8_t *mip_va;
    volatile uint8_t *apb_va;
    volatile uint8_t *table_va;
    volatile uint8_t *pba_va;
    uint32_t rc_span;
    uint32_t mip_span;
    uint32_t apb_span;
    uint32_t table_span;
    uint32_t pba_span;
};

int aarch64_dtb_parse_rp1_irq(const void *blob, uint32_t size,
                              uint64_t selected_gicd, uint64_t console_pa,
                              struct aarch64_rp1_irq_desc *out);

const struct aarch64_rp1_irq_desc *aarch64_rp1_irq_desc_get(void);

int aarch64_rp1_pcie_endpoint(const struct aarch64_rp1_irq_desc *desc,
                              const struct aarch64_rp1_pcie_io *io,
                              struct aarch64_rp1_endpoint *out);

void aarch64_rp1_pcie_prepare(const struct boot_context *ctx);

const struct aarch64_rp1_irq_platform *aarch64_rp1_irq_platform_get(void);

int aarch64_rp1_mmio_read32(const struct aarch64_rp1_irq_platform *p,
                            enum aarch64_rp1_mmio_space space,
                            uint32_t off, uint32_t *value);

int aarch64_rp1_mmio_write32(const struct aarch64_rp1_irq_platform *p,
                             enum aarch64_rp1_mmio_space space,
                             uint32_t off, uint32_t value);

int aarch64_rp1_cfg_read16(const struct aarch64_rp1_irq_platform *p,
                           uint16_t off, uint16_t *value);

int aarch64_rp1_cfg_write16(const struct aarch64_rp1_irq_platform *p,
                            uint16_t off, uint16_t value);

int aarch64_rp1_irq_flush(const struct aarch64_rp1_irq_platform *p,
                          uint32_t source);

#endif /* ARCH_AARCH64_RP1_PCIE_H */
