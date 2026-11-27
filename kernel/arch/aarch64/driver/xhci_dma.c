/* kernel/arch/aarch64/driver/xhci_dma.c — Non-coherent RP1 xHCI DMA ops */
#include <arch/aarch64/rp1_xhci.h>
#include <driver/xhci_dma.h>
#include <errno.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <stdint.h>
#include <string.h>

struct rp1_dma_ctx {
    uint64_t min_pa;
    uint64_t limit_pa;
};

static struct rp1_dma_ctx s_rp1_dma_ctx;

static inline size_t aarch64_dcache_line_size(void)
{
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    uint64_t ctr = 0ULL;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    uint32_t log2_words = (uint32_t)((ctr >> 16) & 0xFU);
    return (size_t)4U << log2_words;
#else
    return 64U;
#endif
}

static int rp1_dma_alloc(void *ctx, size_t bytes, size_t align, struct xhci_dma_buf *out)
{
    const struct rp1_dma_ctx *d = (const struct rp1_dma_ctx *)ctx;
    if (!d || d->min_pa >= d->limit_pa || !out || bytes == 0U)
        return -EINVAL;

    if (bytes <= 4096U && align <= 4096U) {
        uint64_t pa = alloc_4k_page_in_range(d->min_pa, d->limit_pa);
        if (!pa)
            return -ENOMEM;
        out->cpu = Phy_To_Virt(pa);
        out->phys = pa;
        out->bus = pa;
        out->size = bytes;
        out->cookie = NULL;
        return 0;
    }

    if (bytes <= 65536U && align <= 65536U) {
        struct Page *pg = alloc_pages_in_range(ZONE_NORMAL, 1, PG_Kernel,
                                               d->min_pa, d->limit_pa);
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

    return -EINVAL;
}

static void rp1_dma_free(void *ctx, struct xhci_dma_buf *buf)
{
    (void)ctx;
    if (!buf || !buf->phys)
        return;
    if (!buf->cookie && buf->size <= 4096U) {
        free_4k_page(buf->phys);
        return;
    }
    if (buf->cookie)
        free_pages((struct Page *)buf->cookie, 1);
}

static void rp1_dma_sync(void *ctx, void *cpu, size_t size, enum xhci_dma_sync_op op)
{
    (void)ctx;
    if (!cpu || size == 0U)
        return;

#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    size_t line = aarch64_dcache_line_size();
    uintptr_t start = (uintptr_t)cpu & ~(uintptr_t)(line - 1U);
    uintptr_t end = ((uintptr_t)cpu + size + line - 1U) & ~(uintptr_t)(line - 1U);

    for (uintptr_t addr = start; addr < end; addr += line) {
        switch (op) {
        case XHCI_DMA_TO_DEVICE:
            __asm__ __volatile__("dc cvac, %0" :: "r"(addr) : "memory");
            break;
        case XHCI_DMA_PREPARE_FROM_DEVICE:
            __asm__ __volatile__("dc civac, %0" :: "r"(addr) : "memory");
            break;
        case XHCI_DMA_COMPLETE_FROM_DEVICE:
            __asm__ __volatile__("dc ivac, %0" :: "r"(addr) : "memory");
            break;
        }
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    (void)op;
    __sync_synchronize();
#endif
}

static const struct xhci_dma_ops s_rp1_dma_ops = {
    .alloc = rp1_dma_alloc,
    .free = rp1_dma_free,
    .sync = rp1_dma_sync,
};

void aarch64_rp1_xhci_dma_set_window(uint64_t min_pa, uint64_t limit_pa)
{
    s_rp1_dma_ctx.min_pa = min_pa;
    s_rp1_dma_ctx.limit_pa = limit_pa;
}

void *aarch64_rp1_xhci_dma_ctx(void)
{
    return &s_rp1_dma_ctx;
}

const struct xhci_dma_ops *aarch64_rp1_xhci_dma_ops(void)
{
    return &s_rp1_dma_ops;
}
