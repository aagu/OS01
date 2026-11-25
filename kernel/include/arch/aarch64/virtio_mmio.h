#ifndef OS01_AARCH64_VIRTIO_MMIO_H
#define OS01_AARCH64_VIRTIO_MMIO_H

struct boot_context;

/* Map every DTB-discovered virtio-mmio slot's [base, base + 0x200)
 * region into TTBR1 as 2 MiB Device-nGnRnE blocks before SMP starts.
 * Returns 0 on success (including when 0 slots exist), negative on
 * invalid handoff, RAM overlap, or page-table conflict. */
int aarch64_virtio_mmio_prepare(const struct boot_context *ctx);

#endif /* OS01_AARCH64_VIRTIO_MMIO_H */
