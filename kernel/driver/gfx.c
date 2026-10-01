// kernel/driver/gfx.c — /dev/gfx0 — bounded framebuffer present device
//
// Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
// §3 ("Components and kernel interfaces") + §4 ("ABI and userspace
// API").  Task 2 of the gfx 2D API plan.
//
// Concurrency
//   - The 16-entry view table is protected by g_gfx_table.lock.
//   - open() does NOT touch the table; it just allocates a
//     per-file gfx_view.  The first GFX_CREATE_VIEW that succeeds
//     assigns one slot.
//   - The lock covers allocation, configuration, and release only.
//     GFX_PRESENT snapshots the immutable view rect under the
//     lock, then DROPS the lock before any syscall_check_user_range
//     or copy_*_ft call — those primitives may longjmp on a user
//     fault and holding a spinlock across a longjmp would leak it.
//   - release_file() runs from file_free (Task 1 dispatch path),
//     invoked exactly once when the last file_put drops the
//     refcount to zero.  It clears the slot under the lock and
//     frees the gfx_view struct.
//
// Memory safety
//   - Every ioctl argument (gfx_view_desc_t, gfx_info_t,
//     gfx_present_req_t) is staged through syscall_check_user_range
//     + copy_*_ft; no struct is dereferenced directly.  An unconfigured
//     view returns EINVAL for GET_INFO / PRESENT.
//   - GFX_PRESENT walks the view row-by-row.  The heap row buffer is
//     freed on EVERY return path; a fault on row N leaves the first
//     N-1 rows visible (spec §4: "前面行可能已更新，下一次完整
//     present 可恢复").  The fb_write_row validation rejects an
//     out-of-range row before the first copy_from_user_ft call so
//     a hostile caller can't escape the view rectangle.
//
// What this does NOT do
//   - No mmap callback (userspace hands in pixel buffers per
//     present; the kernel never maps them).
//   - No vsync / page-flip / hardware double buffering.
//   - No per-process / per-view isolation beyond the device's
//     open/close model.  /dev/fb remains a kernel-side-bypass that
//     other tasks can use; that risk is acknowledged in spec §2 and
//     parked for the window-server phase.

#include <driver/gfx.h>
#include <driver/fb.h>
#include <uapi/gfx.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <memory/uaccess.h>      // syscall_check_user_range, copy_*_ft
#include <memory/slab.h>         // kmalloc, kfree
#include <arch/spinlock.h>       // spinlock_T, spin_lock_irqsave
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>              // free — file_alloc uses calloc, not kmalloc

// ── Per-file view state is declared in kernel/include/driver/gfx.h
//    so the test TU can reference the struct.  Here we only operate
//    on the slots table.

// ── Global view table ───────────────────────────────────────
// 16 fixed slots (spec §3 — 固定视图表最多 16 个).  Each slot is
// either NULL (free) or points at a gfx_view_t (configured).  The
// gfx_view_t itself is owned by the open file (f->dev_private) so
// release_file can match the slot clearing with the free.
typedef struct gfx_view_table {
    spinlock_T lock;
    gfx_view_t *slots[GFX_MAX_VIEWS];
} gfx_view_table_t;

static gfx_view_table_t g_gfx_table;

// ── Init ───────────────────────────────────────────────────
// Called from x86_64_boot_device_nodes (after devfs_init so the
// registration slot is open).  Lazily re-runnable; subsequent calls
// are no-ops.
void gfx_init(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;
    spin_init(&g_gfx_table.lock);
    for (int i = 0; i < GFX_MAX_VIEWS; i++) g_gfx_table.slots[i] = NULL;
}

// ── gfx_open: allocate the per-file view struct ─────────────
// Returns a custom FD_DEV file_t whose dev_private points at a
// zeroed gfx_view.  No slot in the global table is reserved here;
// that happens on the first successful GFX_CREATE_VIEW.
static int gfx_open(const char *name, file_t **out_file)
{
    (void)name;
    if (!out_file) return -EINVAL;

    file_t *f = file_alloc();
    if (!f) return -ENOMEM;

    gfx_view_t *v = (gfx_view_t *)kmalloc(sizeof(*v));
    if (!v) {
        // file_alloc uses calloc(1, sizeof(file_t)) — bare calloc,
        // not kmalloc.  Use the production file_free so vfs_node_put
        // (NULL — file_alloc zeroed node) and the slab-free stay
        // symmetric with the success path.
        free(f);
        return -ENOMEM;
    }
    memset(v, 0, sizeof(*v));
    v->slot = -1;
    v->configured = false;

    f->dev_private = v;
    *out_file = f;
    return 0;
}

// ── gfx_release_file: free the slot + view ──────────────────
// Fires from file_free (Task 1) on the final file_put.  We do
// NOT free(f) here — file_free does that immediately after.
static void gfx_release_file(file_t *f)
{
    if (!f || !f->dev_private) return;
    gfx_view_t *v = (gfx_view_t *)f->dev_private;

    uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
    if (v->slot >= 0 && v->slot < GFX_MAX_VIEWS &&
        g_gfx_table.slots[v->slot] == v) {
        g_gfx_table.slots[v->slot] = NULL;
    }
    v->slot = -1;
    v->configured = false;
    spin_unlock_irqrestore(&g_gfx_table.lock, flags);

    kfree(v);
    f->dev_private = NULL;
}

// ── gfx_ioctl_create_view ──────────────────────────────────
// Validate the view dimensions against the live framebuffer, then
// find a free slot and bind it.  Allocated slot + configured fields
// are stamped under the table lock; the caller's file is the only
// window into the slot for the duration.
static int gfx_ioctl_create_view(file_t *f, const gfx_view_desc_t *kdesc)
{
    gfx_view_t *v = (gfx_view_t *)f->dev_private;
    if (!v || !kdesc) return -EINVAL;

    struct fb_info info;
    int grc = fb_get_info(&info);
    if (grc < 0) return grc;

    // Spec §3 — only RGB32 framebuffers are supported.  A different
    // bpp / format means libgfx's stride assumption (width*4) would
    // be wrong, and silently accepting it would corrupt the fb.
    if (info.format != GFX_FORMAT_RGB32) return -EINVAL;

    uint32_t fb_w = info.width;
    uint32_t fb_h = info.height;

    // Spec §3 — w, h non-zero.  Spec §4 — overflow-safe subtraction
    // form so UINT32_MAX w / h is rejected even when x or y is 0.
    if (kdesc->w == 0 || kdesc->h == 0) return -EINVAL;
    if (kdesc->w > UINT32_MAX / 4u)    return -EINVAL;
    if (kdesc->x > fb_w)              return -EINVAL;
    if (kdesc->w > fb_w - kdesc->x)   return -EINVAL;
    if (kdesc->y > fb_h)              return -EINVAL;
    if (kdesc->h > fb_h - kdesc->y)   return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);

    // Reconfigure rejection — the spec says the view is bound to a
    // single (x,y,w,h) rectangle for its lifetime; a second
    // GFX_CREATE_VIEW on the same file returns EINVAL.
    if (v->configured) {
        spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        return -EINVAL;
    }

    int slot = -1;
    for (int i = 0; i < GFX_MAX_VIEWS; i++) {
        if (g_gfx_table.slots[i] == NULL) { slot = i; break; }
    }
    if (slot < 0) {
        // 16 slots full — libgfx maps this to errno=EMFILE per spec §5.
        spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        return -EMFILE;
    }

    g_gfx_table.slots[slot] = v;
    v->slot        = slot;
    v->desc_x      = kdesc->x;
    v->desc_y      = kdesc->y;
    v->desc_w      = kdesc->w;
    v->desc_h      = kdesc->h;
    v->format      = GFX_FORMAT_RGB32;
    v->configured  = true;

    spin_unlock_irqrestore(&g_gfx_table.lock, flags);
    return 0;
}

// ── gfx_ioctl_get_info ─────────────────────────────────────
// Snapshot under lock, then drop the lock and write the result back
// to userspace via copy_to_user_ft.  An unconfigured view returns
// EINVAL so libgfx can map that to errno.
static int gfx_ioctl_get_info(file_t *f, gfx_info_t *kinfo)
{
    gfx_view_t *v = (gfx_view_t *)f->dev_private;
    if (!v || !kinfo) return -EINVAL;

    uint32_t w, h, format;
    {
        uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
        if (!v->configured) {
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
            return -EINVAL;
        }
        w      = v->desc_w;
        h      = v->desc_h;
        format = v->format;
        spin_unlock_irqrestore(&g_gfx_table.lock, flags);
    }

    kinfo->width  = w;
    kinfo->height = h;
    kinfo->stride = w * 4u;
    kinfo->format = format;
    return 0;
}

// ── gfx_ioctl_present ──────────────────────────────────────
// Spec §4: present validates the request struct's user range,
// snapshots the view rect under the lock, drops the lock, then
// walks the view row-by-row.  Each row:
//   (a) re-checks the per-row user sub-range with
//       syscall_check_user_range (snapshot, not a pin),
//   (b) fault-tolerant copies the row bytes into a heap row buffer,
//   (c) calls fb_write_row(x, y + row, buf, stride).
//
// On any fault, kfree the row buffer and return -EFAULT — earlier
// rows may already be visible on the fb (spec §4: "前面行可能已
// 更新，下一次完整 present 可恢复").
static int gfx_ioctl_present(file_t *f, gfx_present_req_t *ureq)
{
    gfx_view_t *v = (gfx_view_t *)f->dev_private;
    if (!v || !ureq) return -EINVAL;

    // Stage 1: validate + copy the request struct.
    if (!syscall_check_user_range((uint64_t)ureq,
                                  sizeof(gfx_present_req_t), false))
        return -EFAULT;
    gfx_present_req_t kreq;
    if (copy_from_user_ft(&kreq, ureq, sizeof(kreq)) < 0)
        return -EFAULT;

    // Stage 2: snapshot the view rectangle under the table lock.
    // The lock is dropped before any user-range check or copy_*_ft
    // call (those primitives may longjmp on user fault; holding a
    // spinlock across a longjmp would leak it).
    uint32_t v_x, v_y, v_w, v_h;
    {
        uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
        if (!v->configured) {
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
            return -EINVAL;
        }
        v_x = v->desc_x;
        v_y = v->desc_y;
        v_w = v->desc_w;
        v_h = v->desc_h;
        spin_unlock_irqrestore(&g_gfx_table.lock, flags);
    }

    // Stage 3: validate stride / reserved / total byte count.  The
    // multiplication MUST be done in uint64_t — v_h * stride is at
    // most 2^32 * 2^32 (overflow), which would otherwise let a
    // hostile stride pass a malformed length to syscall_check_user_range.
    uint32_t expected_stride = v_w * 4u;
    if (kreq.stride != expected_stride) return -EINVAL;
    if (kreq.reserved != 0)             return -EINVAL;
    if (kreq.pixels == 0)               return -EFAULT;
    if (v_h != 0) {
        uint64_t total_bytes = (uint64_t)v_h * (uint64_t)kreq.stride;
        if (total_bytes > (uint64_t)SIZE_MAX) return -EINVAL;
        if (!syscall_check_user_range(kreq.pixels, total_bytes, false))
            return -EFAULT;
    }

    // Stage 4: walk the rows.  Allocate a single heap row buffer of
    // exactly stride bytes; free it on every return path.
    size_t row_buf_size = (size_t)kreq.stride;
    void *row_buf = kmalloc(row_buf_size);
    if (!row_buf) return -ENOMEM;

    for (uint32_t row = 0; row < v_h; row++) {
        uint64_t row_addr = kreq.pixels + (uint64_t)row * (uint64_t)kreq.stride;
        if (!syscall_check_user_range(row_addr, (uint64_t)kreq.stride, false)) {
            kfree(row_buf);
            return -EFAULT;
        }
        if (copy_from_user_ft(row_buf, (const void *)(uintptr_t)row_addr,
                              (size_t)kreq.stride) < 0) {
            kfree(row_buf);
            return -EFAULT;
        }
        // fb_write_row validates against fb dimensions / FB_length
        // (kernel/driver/fb.c).  A negative return here means our
        // view snapshot drifted out of range — defensive, since the
        // spec doesn't expect this to fire after the validate step.
        int wrc = fb_write_row(v_x, v_y + row, row_buf, (uint32_t)kreq.stride);
        if (wrc < 0) {
            kfree(row_buf);
            return wrc;
        }
    }

    kfree(row_buf);
    return 0;
}

// ── gfx_ioctl_file — Task 1 dispatch ────────────────────────
// This is the ops->ioctl_file callback; devfs_ioctl_file prefers
// it over the node-only callback.  Every command stages its
// argument via syscall_check_user_range + copy_*_ft (no direct
// dereference of a user pointer).
static int gfx_ioctl_file(file_t *f, int cmd, void *arg)
{
    if (!f) return -EINVAL;

    switch (cmd) {
    case GFX_CREATE_VIEW: {
        if (!arg) return -EINVAL;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(gfx_view_desc_t), false))
            return -EFAULT;
        gfx_view_desc_t kdesc;
        if (copy_from_user_ft(&kdesc, arg, sizeof(kdesc)) < 0)
            return -EFAULT;
        return gfx_ioctl_create_view(f, &kdesc);
    }
    case GFX_GET_INFO: {
        if (!arg) return -EINVAL;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(gfx_info_t), true))
            return -EFAULT;
        gfx_info_t kinfo;
        int grc = gfx_ioctl_get_info(f, &kinfo);
        if (grc < 0) return grc;
        if (copy_to_user_ft(arg, &kinfo, sizeof(kinfo)) < 0)
            return -EFAULT;
        return 0;
    }
    case GFX_PRESENT: {
        // gfx_ioctl_present itself does syscall_check_user_range +
        // copy_from_user_ft on the request struct.
        return gfx_ioctl_present(f, (gfx_present_req_t *)arg);
    }
    default:
        return -ENOTTY;
    }
}

// ── Public devfs ops table (kernel/include/driver/gfx.h) ────
// No mmap / no read / no write — gfx0 is ioctl-only.
const struct devfs_ops gfx_ops = {
    .open         = gfx_open,
    .ioctl_file   = gfx_ioctl_file,
    .release_file = gfx_release_file,
};
