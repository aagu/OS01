#ifndef _DRIVER_VIRTIO_BLK_H
#define _DRIVER_VIRTIO_BLK_H

/*
 * driver/virtio_blk.h — architecture-neutral virtio-blk driver over
 * virtio-mmio.
 *
 * The driver self-registers as a subsys in SUBSYS_PHASE_STORAGE and
 * probes each platform slot returned by arch_virtio_mmio_slot_count() /
 * arch_virtio_mmio_slot_get(), registering one BLOCK_DISK per block
 * device.  Transfers use synchronous request/response over split
 * virtqueue 0 with polling on the used ring.
 */

#include <stdint.h>

/* 0 on success (>=0 devices probed may still be 0 when the platform
 * exposes no virtio-blk); negative on mapping/probe failure. */
int virtio_blk_init(void);

#endif /* _DRIVER_VIRTIO_BLK_H */
