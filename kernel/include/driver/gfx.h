#ifndef _KERNEL_GFX_H
#define _KERNEL_GFX_H

// kernel/include/driver/gfx.h — /dev/gfx0 — bounded framebuffer present device
//
// Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md) §3+§4:
//   * Each open(2) on /dev/gfx0 produces an unconfigured gfx_view bound
//     to the file via file->dev_private.  No slot in the 16-entry view
//     table is reserved until the first GFX_CREATE_VIEW ioctl succeeds.
//   * GFX_CREATE_VIEW validates x/y/w/h against the live framebuffer
//     dimensions and the open view's "not already configured" state,
//     then allocates one slot under a small spinlock.
//   * GFX_GET_INFO / GFX_PRESENT on an unconfigured view return EINVAL.
//   * GFX_PRESENT snapshots the immutable view rectangle under the
//     lock, drops the lock, then validates the per-call request and
//     copies row-by-row through copy_from_user_ft into a kernel row
//     buffer, calling fb_write_row on each row.  The view-table lock
//     is NEVER held across syscall_check_user_range / copy_from_user_ft
//     (those may longjmp on user-fault).
//   * release_file (Task 1 dispatch path) frees the view struct and
//     clears the slot.  No mmap callback is registered.

#include <stdint.h>
#include <stdbool.h>
#include <uapi/gfx.h>

// Forward declarations — the headers below are NOT included here so this
// header can be pulled into the host test runtime without dragging in the
// full kernel header chain.  The implementation (kernel/driver/gfx.c) and
// any kernel caller that needs the full types includes them itself.
struct file;
struct devfs_ops;

// Per-file view state.  Lives in file->dev_private, allocated on
// gfx_open and freed on gfx_release_file (Task 1 devfs_release_file
// dispatch).  `slot` is -1 until GFX_CREATE_VIEW succeeds; thereafter
// it indexes the global gfx_view_table[] slot array (kernel/driver/gfx.c).
typedef struct gfx_view {
    int      slot;           // -1 or 0..GFX_MAX_VIEWS-1
    uint32_t desc_x;
    uint32_t desc_y;
    uint32_t desc_w;
    uint32_t desc_h;
    uint32_t format;
    uint64_t mode_seq;
    bool     configured;
} gfx_view_t;

// Maximum simultaneous views (spec §3 — 固定视图表最多 16 个).
#define GFX_MAX_VIEWS 16

// Initialize the 16-entry view table (spinlock + NULL slots).  Must
// be called before devfs_register_chrdev("gfx0", ...).  Idempotent —
// re-runs are no-ops (the kernel calls this once from
// x86_64_boot_device_nodes()).
void gfx_init(void);

// /dev/gfx0 devfs ops table (defined in kernel/driver/gfx.c).
extern const struct devfs_ops gfx_ops;

#endif /* _KERNEL_GFX_H */