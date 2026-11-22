#ifndef _ARCH_VIRTIO_MMIO_H
#define _ARCH_VIRTIO_MMIO_H

/*
 * arch/virtio_mmio.h — architecture facade for virtio-mmio discovery
 * and early MMIO window mapping.
 *
 * The virtio-blk driver in kernel/driver/virtio_blk.c is arch-neutral;
 * each architecture (e.g. aarch64 DTB + TTBR1 early map) provides strong
 * overrides for these hooks when it exposes a virtio-mmio bus.
 * Architectures without virtio-mmio fall back to the weak no-op stubs.
 */

#include <stdint.h>

struct arch_virtio_mmio_slot {
    uint64_t base;
    uint32_t irq;
};

/* Map the platform's virtio-mmio MMIO window during pre-SMP boot if
 * required by the architecture's direct map.  Idempotent; returns 0 on
 * success or when no early mapping is needed. */
int arch_virtio_mmio_map_early(void);

/* Number of virtio-mmio slots discovered by the platform. */
uint32_t arch_virtio_mmio_slot_count(void);

/* Read slot `index` into `*out`.  Returns 0 on success, -ENOENT if out
 * of range. */
int arch_virtio_mmio_slot_get(uint32_t index,
                              struct arch_virtio_mmio_slot *out);

#endif /* _ARCH_VIRTIO_MMIO_H */
