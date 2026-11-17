#ifndef OS01_AARCH64_VIRTIO_BLK_H
#define OS01_AARCH64_VIRTIO_BLK_H

/*
 * virtio-blk over virtio-mmio (QEMU virt) — the aarch64 BLOCK_DISK
 * transport (M4-remaining "aarch64 opens the VFS gate").
 *
 * The driver self-registers as a subsys in SUBSYS_PHASE_STORAGE (the same
 * phase x86's device-boot uses) and registers one block device per
 * virtio,mmio DTB slot whose MAGIC/VERSION/DEVICEID probe answers as a
 * block device.  Transfers are synchronous request/response over split
 * virtqueue 0 with polling on the used ring (no IRQ yet).
 */

#include <stdint.h>

/* 0 on success (>=0 devices probed may still be 0 when QEMU exposes no
 * virtio-blk); negative on mapping/probe-environment failure. */
int virtio_blk_init(void);

#endif /* OS01_AARCH64_VIRTIO_BLK_H */
