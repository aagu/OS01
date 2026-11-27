/* kernel/include/driver/xhci_dma.h — xHCI DMA buffer and cache-sync abstraction */
#ifndef _DRIVER_XHCI_DMA_H
#define _DRIVER_XHCI_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xhci_controller;

enum xhci_dma_sync_op {
    XHCI_DMA_TO_DEVICE = 0,
    XHCI_DMA_PREPARE_FROM_DEVICE = 1,
    XHCI_DMA_COMPLETE_FROM_DEVICE = 2,
};

struct xhci_dma_buf {
    void *cpu;
    uint64_t phys;
    uint64_t bus;
    size_t size;
    void *cookie;
};

struct xhci_dma_ops {
    int (*alloc)(void *ctx, size_t bytes, size_t align, struct xhci_dma_buf *out);
    void (*free)(void *ctx, struct xhci_dma_buf *buf);
    void (*sync)(void *ctx, void *cpu, size_t size, enum xhci_dma_sync_op op);
};

int xhci_dma_alloc(struct xhci_controller *ctrl, size_t bytes, size_t align,
                   size_t boundary, struct xhci_dma_buf *out);
void xhci_dma_free(struct xhci_controller *ctrl, struct xhci_dma_buf *buf);
void xhci_dma_sync(const struct xhci_controller *ctrl, const struct xhci_dma_buf *buf,
                   size_t offset, size_t size, enum xhci_dma_sync_op op);
bool xhci_dma_addr_ok(const struct xhci_controller *ctrl, uint64_t bus,
                      size_t bytes, size_t align, size_t boundary);

#endif /* _DRIVER_XHCI_DMA_H */
