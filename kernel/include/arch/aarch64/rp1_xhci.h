#ifndef ARCH_AARCH64_RP1_XHCI_H
#define ARCH_AARCH64_RP1_XHCI_H

#include <core/bootinfo.h>
#include <driver/xhci_dma.h>
#include <stdbool.h>
#include <stdint.h>

#define AARCH64_RP1_MAX_XHCI      2U
#define AARCH64_RP1_USB_SPAN      0x100000ULL
#define AARCH64_RP1_XHCI_MMIO_SIZE 0x8000ULL
#define AARCH64_RP1_DMA_HARD_LIMIT 0xC0000000ULL

struct aarch64_rp1_xhci_slot {
    bool valid;
    uint64_t rp1_offset;
    uint64_t size;
};

struct aarch64_rp1_xhci_desc {
    bool valid;
    uint64_t rc_reg_base;
    uint64_t rc_reg_size;
    uint64_t rp1_child_base;
    uint32_t controller_count;
    struct aarch64_rp1_xhci_slot controllers[AARCH64_RP1_MAX_XHCI];
};

struct aarch64_rp1_pcie_io {
    void *ctx;
    uint32_t (*rc_read32)(void *ctx, uint32_t offset);
    uint32_t (*cfg_save_select)(void *ctx, uint32_t bdf);
    void (*cfg_restore)(void *ctx, uint32_t saved);
};

struct aarch64_rp1_pcie_resources {
    bool valid;
    uint64_t bar1_cpu_base;
    uint64_t bar1_pci_base;
    uint64_t dma_min_pa;
    uint64_t dma_limit_pa;
    uint64_t outbound_cpu_base;
    uint64_t outbound_cpu_limit;
    uint8_t secondary_bus;
};

int aarch64_dtb_parse_rp1_xhci(const void *blob, uint32_t size,
                               struct aarch64_rp1_xhci_desc *out);

const struct aarch64_rp1_xhci_desc *aarch64_rp1_xhci_desc_get(void);

int aarch64_rp1_pcie_resources(const struct aarch64_rp1_xhci_desc *desc,
                               const struct aarch64_rp1_pcie_io *io,
                               struct aarch64_rp1_pcie_resources *out);

int aarch64_rp1_xhci_prepare(const struct boot_context *ctx);

const struct xhci_dma_ops *aarch64_rp1_xhci_dma_ops(void);

#endif /* ARCH_AARCH64_RP1_XHCI_H */
