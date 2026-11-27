#ifndef ARCH_AARCH64_BOOT_MMIO_H
#define ARCH_AARCH64_BOOT_MMIO_H

#include <core/bootinfo.h>
#include <stdint.h>

struct aarch64_boot_mmio_ctx {
    const void *mmap_ptr;
    uint32_t stride;
    uint32_t desc_count;
    uint64_t *ttbr1_root;
};

int aarch64_boot_mmio_ctx_init(const struct boot_context *boot,
                               struct aarch64_boot_mmio_ctx *out);

int aarch64_boot_mmio_map(const struct aarch64_boot_mmio_ctx *ctx,
                          uint64_t base, uint64_t size);

#endif /* ARCH_AARCH64_BOOT_MMIO_H */
