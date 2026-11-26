#ifndef OS01_AARCH64_UEFI_CACHE_HANDOFF_H
#define OS01_AARCH64_UEFI_CACHE_HANDOFF_H

#include <stdint.h>

struct aarch64_handoff_cache_regions {
    uint64_t boot_block_base;
    uint64_t boot_block_size;
    uint64_t trampoline_base;
    uint64_t page_size;
    uint64_t ctx_phys;
    uint64_t fdt_phys;
    uint64_t fdt_pages;
    uint64_t mmap_phys;
    uint64_t mmap_pages;
};

struct aarch64_cache_ops {
    uint64_t (*read_ctr)(void *ctx);
    void (*dc_civac)(void *ctx, uint64_t addr);
    void (*dsb_sy)(void *ctx);
    void (*ic_iallu)(void *ctx);
    void (*isb)(void *ctx);
};

static inline uint32_t aarch64_ctr_dminline_bytes(uint64_t ctr)
{
    return 4u << ((uint32_t)(ctr >> 16) & 0xfu);
}

static inline void aarch64_clean_invalidate_dcache_range(
    uint64_t base, uint64_t size, uint32_t line,
    const struct aarch64_cache_ops *ops, void *ctx)
{
    if (!size || !ops || !ops->dc_civac)
        return;
    if (line == 0u || (line & (line - 1u)) != 0u)
        line = 64u;
    uint64_t mask = (uint64_t)(line - 1u);
    uint64_t addr = base & ~mask;
    uint64_t end = (base + size + mask) & ~mask;

    for (; addr < end; addr += line)
        ops->dc_civac(ctx, addr);
}

static inline void aarch64_flush_handoff_caches(
    const struct aarch64_handoff_cache_regions *r,
    const struct aarch64_cache_ops *ops, void *ctx)
{
    if (!r || !ops || !ops->read_ctr)
        return;
    uint32_t line = aarch64_ctr_dminline_bytes(ops->read_ctr(ctx));

    aarch64_clean_invalidate_dcache_range(r->boot_block_base,
                                          r->boot_block_size, line, ops, ctx);
    aarch64_clean_invalidate_dcache_range(r->trampoline_base,
                                          r->page_size, line, ops, ctx);
    if (r->ctx_phys != 0u)
        aarch64_clean_invalidate_dcache_range(r->ctx_phys,
                                              r->page_size, line, ops, ctx);
    if (r->fdt_phys != 0u && r->fdt_pages != 0u)
        aarch64_clean_invalidate_dcache_range(r->fdt_phys,
                                              r->fdt_pages * r->page_size,
                                              line, ops, ctx);
    if (r->mmap_phys != 0u && r->mmap_pages != 0u)
        aarch64_clean_invalidate_dcache_range(r->mmap_phys,
                                              r->mmap_pages * r->page_size,
                                              line, ops, ctx);
    if (ops->dsb_sy)
        ops->dsb_sy(ctx);
    if (ops->ic_iallu)
        ops->ic_iallu(ctx);
    if (ops->dsb_sy)
        ops->dsb_sy(ctx);
    if (ops->isb)
        ops->isb(ctx);
}

#endif /* OS01_AARCH64_UEFI_CACHE_HANDOFF_H */
