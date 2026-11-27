/* kernel/driver/xhci_dma.c — xHCI DMA allocation, boundary checks, and sync */
#include <driver/xhci.h>
#include <driver/xhci_dma.h>
#include <errno.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <string.h>

static inline void xhci_dma_barrier(void)
{
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static bool is_pow2_or_zero(size_t v)
{
    return (v & (v - 1U)) == 0;
}

bool xhci_dma_addr_ok(const struct xhci_controller *ctrl, uint64_t bus,
                      size_t bytes, size_t align, size_t boundary)
{
    if (bus == 0 || bytes == 0)
        return false;
    if (align > 1U) {
        if (!is_pow2_or_zero(align))
            return false;
        if ((bus & (uint64_t)(align - 1U)) != 0)
            return false;
    }
    if ((uint64_t)(bytes - 1U) > UINT64_MAX - bus)
        return false;

    uint64_t end = bus + (uint64_t)(bytes - 1U);
    bool ac64 = true;
    if (ctrl) {
        if (ctrl->ac64_valid) {
            ac64 = ctrl->ac64;
        } else if (ctrl->mmio_virt) {
            ac64 = (xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCCPARAMS1) & 1U) != 0;
        }
    }
    if (!ac64 && end > 0xFFFFFFFFULL)
        return false;

    if (boundary != 0) {
        if (!is_pow2_or_zero(boundary) || bytes > boundary)
            return false;
        uint64_t mask = ~((uint64_t)boundary - 1U);
        if ((bus & mask) != (end & mask))
            return false;
    }
    return true;
}

static int default_dma_alloc(size_t bytes, size_t align, struct xhci_dma_buf *out)
{
    if (bytes <= 4096U && align <= 4096U) {
        uint64_t pa = alloc_4k_page();
        if (!pa)
            return -ENOMEM;
        out->cpu = Phy_To_Virt(pa);
        out->phys = pa;
        out->bus = pa;
        out->size = bytes;
        out->cookie = NULL;
        return 0;
    }

#ifndef OS01_HOST_TEST
    if (bytes <= 65536U && align <= 65536U) {
        uint64_t nr_pages = (bytes + PAGE_2M_SIZE - 1U) / PAGE_2M_SIZE;
        struct Page *pg = alloc_pages(ZONE_NORMAL, nr_pages, PG_Kernel);
        if (!pg)
            return -ENOMEM;
        uint64_t pa = pg->phy_address;
        out->cpu = Phy_To_Virt(pa);
        out->phys = pa;
        out->bus = pa;
        out->size = bytes;
        out->cookie = pg;
        return 0;
    }
#else
    if (bytes <= 65536U) {
        size_t eff_align = align < 4096U ? 4096U : align;
        size_t alloc_sz = (bytes + eff_align - 1U) & ~(eff_align - 1U);
        void *va = aligned_alloc(eff_align, alloc_sz);
        if (!va)
            return -ENOMEM;
        out->cpu = va;
        out->phys = (uint64_t)(uintptr_t)va;
        out->bus = (uint64_t)(uintptr_t)va;
        out->size = bytes;
        out->cookie = va;
        return 0;
    }
#endif
    return -EINVAL;
}

static void default_dma_free(struct xhci_dma_buf *buf)
{
    if (!buf || !buf->phys)
        return;
    if (!buf->cookie && buf->size <= 4096U) {
        free_4k_page(buf->phys);
        return;
    }
#ifndef OS01_HOST_TEST
    if (buf->cookie) {
        uint64_t nr_pages = (buf->size + PAGE_2M_SIZE - 1U) / PAGE_2M_SIZE;
        free_pages((struct Page *)buf->cookie, nr_pages);
    }
#else
    free(buf->cookie);
#endif
}

int xhci_dma_alloc(struct xhci_controller *ctrl, size_t bytes, size_t align,
                   size_t boundary, struct xhci_dma_buf *out)
{
    if (!out || bytes == 0)
        return -EINVAL;
    if (align == 0)
        align = 1U;
    if (!is_pow2_or_zero(align))
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    int rc;
    if (ctrl && ctrl->dma_ops && ctrl->dma_ops->alloc) {
        rc = ctrl->dma_ops->alloc(ctrl->dma_ctx, bytes, align, out);
    } else {
        rc = default_dma_alloc(bytes, align, out);
    }
    if (rc != 0)
        return rc;

    if (!out->cpu || !out->phys || out->size < bytes ||
        !xhci_dma_addr_ok(ctrl, out->bus, bytes, align, boundary)) {
        xhci_dma_free(ctrl, out);
        return -EIO;
    }
    memset(out->cpu, 0, bytes);
    return 0;
}

void xhci_dma_free(struct xhci_controller *ctrl, struct xhci_dma_buf *buf)
{
    if (!buf)
        return;
    if (buf->cpu || buf->phys) {
        if (ctrl && ctrl->dma_ops && ctrl->dma_ops->free) {
            ctrl->dma_ops->free(ctrl->dma_ctx, buf);
        } else {
            default_dma_free(buf);
        }
    }
    memset(buf, 0, sizeof(*buf));
}

void xhci_dma_sync(const struct xhci_controller *ctrl, const struct xhci_dma_buf *buf,
                   size_t offset, size_t size, enum xhci_dma_sync_op op)
{
    if (!buf || !buf->cpu || size == 0)
        return;
    if (offset > buf->size || size > buf->size - offset)
        return;

    void *addr = (uint8_t *)buf->cpu + offset;
    if (ctrl && ctrl->dma_ops && ctrl->dma_ops->sync) {
        ctrl->dma_ops->sync(ctrl->dma_ctx, addr, size, op);
    } else {
        xhci_dma_barrier();
    }
}
