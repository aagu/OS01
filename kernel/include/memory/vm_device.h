#ifndef _KERNEL_VM_DEVICE_H
#define _KERNEL_VM_DEVICE_H

#include <stdint.h>

/* ── Semantic device-mapping request (mm-architecture-boundaries) ──
 *
 * A device mmap request crosses the device → VFS → MM boundary.  The
 * request carries SEMANTIC information only: the first page's physical
 * address plus which memory class the device pages belong to.  Hardware
 * descriptor encodings (x86 PCD/PWT, ARM descriptor attributes) are
 * derived by the owning architecture's commit side, so no PTE word can
 * leak across the device/VFS/MM boundary.
 *
 *   UC (uncacheable): device registers, and every framebuffer whose
 *                     backend has not identified ordinary RAM.
 *   WB (write-back):  ordinary RAM behind a device BAR.
 */
enum vm_device_cache {
    VM_DEVICE_CACHE_UC = 0,
    VM_DEVICE_CACHE_WB = 1,
};

#endif /* _KERNEL_VM_DEVICE_H */
