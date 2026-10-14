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
#include <driver/fb_state.h>
#include <uapi/gfx.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <sched/task.h>
#include <memory/uaccess.h>      // syscall_check_user_range, copy_*_ft
#include <memory/slab.h>         // kmalloc, kfree
#include <arch/spinlock.h>       // spinlock_T, spin_lock_irqsave
#include <arch/clocksource.h>    // arch_clocksource_read_ns (terminal-cpu probe)
#include <core/printk.h>         // serial_printk                (terminal-cpu probe)
#include <log/log.h>             // log_debug                  (partial-present stats)
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

    f->type = FD_DEV;
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

    // Detach view unconditionally from display coordinator before
    // clearing slot and freeing view memory.
    fb_owner_detach(v);

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

    fb_snapshot_t snap;
    int grc = fb_snapshot_read(&snap);
    if (grc < 0) return grc;

    // Spec §3 — only RGB32 framebuffers are supported.  A different
    // bpp / format means libgfx's stride assumption (width*4) would
    // be wrong, and silently accepting it would corrupt the fb.
    if (snap.state.info.format != GFX_FORMAT_RGB32) return -EINVAL;

    uint32_t fb_w = snap.state.info.width;
    uint32_t fb_h = snap.state.info.height;

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
    v->mode_seq    = snap.state.generation;
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

// ── Partial-present stats (spec §8) ─────────────────────────────
//
// Per-ioctl instrumentation (NOT per-logical-frame: that's the
// display layer's job in Task 3).  Three numbers:
//   - full_success_ns / rect_success_ns : wall time of the success path
//     for full vs rect ioctls (terminal-cpu probe style; logged via
//     log_debug, not serial_printk, per the binding "新代码不新增
//     serial_printk" constraint).
//   - row_bytes_written : cumulative bytes written via fb_write_row_leased
//     across successful rows; on the rect path, per-success-row is w*4.
//     The "prefix" counter folds in the rows a previous FAILED rect
//     present successfully wrote before tripping — a lower bound for
//     the failed attempt's actual write volume, since the fault path
//     can longjmp with the row_count register clobbered.
//   - failed_attempts : any rect present that returned a negative
//     errno (EFAULT, ENOMEM, ESTALE, EACCES, EIO, EINVAL, ...).
//
// All counters are 64-bit atomic loads/stores so SMP racing updates
// don't lose a sample.  The log_debug throttle is 1 Hz (we sample the
// timestamp and skip the print if < 1 s since the previous one).
//
// The longjmp path MUST NOT read setjmp-volatile-changed locals;
// `prefix_bytes_this_attempt` is `volatile` so the compiler keeps the
// store across the longjmp and we can fold it into row_bytes_written
// on the next successful call.

static struct {
    uint64_t full_success_ns;
    uint64_t rect_success_ns;
    uint64_t full_count;
    uint64_t rect_count;
    uint64_t row_bytes_written;          /* full path: stride per row */
    uint64_t prefix_bytes_to_fold;       /* unconsumed success prefix
                                          * from a failed attempt */
    uint64_t failed_attempts;
    uint64_t last_log_ns;
} g_present_stats;

static uint64_t atomic_load_u64(uint64_t *p)
{
    /* __atomic_load_n with __ATOMIC_RELAXED is the cheapest portable
     * primitive; the kernel's own inline asm atomic ops (x86_64 lock;
     * aarch64 ldar) match.  We avoid pulling in arch/spinlock.h's
     * spinlock type because stats counters are not lock-protected
     * invariants — torn mid-update is harmless (one sample lost). */
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static void atomic_add_u64(uint64_t *p, uint64_t v)
{
    __atomic_add_fetch(p, v, __ATOMIC_RELAXED);
}

/* Fold any pending failure-prefix into the cumulative row-byte
 * counter.  Called at the top of every successful present so the
 * bytes a previous failed attempt successfully wrote are accounted
 * for before the new attempt's bytes land.  The volatile
 * read+compare-exchange pattern avoids a torn 0/prefix sequence when
 * a failure on another CPU races. */
static void fold_prefix_bytes(void)
{
    for (;;) {
        uint64_t cur = atomic_load_u64(&g_present_stats.prefix_bytes_to_fold);
        if (cur == 0) return;
        if (__atomic_compare_exchange_n(&g_present_stats.prefix_bytes_to_fold,
                                        &cur, 0, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            atomic_add_u64(&g_present_stats.row_bytes_written, cur);
            return;
        }
    }
}

static void stats_log_maybe(uint64_t now_ns)
{
    uint64_t last = atomic_load_u64(&g_present_stats.last_log_ns);
    /* 1 second minimum spacing.  A torn read could let through two
     * samples within < 1 s — harmless for a stats line. */
    if (now_ns - last < (uint64_t)1000000000ULL) return;
    if (!__atomic_compare_exchange_n(&g_present_stats.last_log_ns,
                                     &last, now_ns, 0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED)) {
        return;     /* someone else logged */
    }

    uint64_t full_ns   = atomic_load_u64(&g_present_stats.full_success_ns);
    uint64_t rect_ns   = atomic_load_u64(&g_present_stats.rect_success_ns);
    uint64_t full_n    = atomic_load_u64(&g_present_stats.full_count);
    uint64_t rect_n    = atomic_load_u64(&g_present_stats.rect_count);
    uint64_t bytes     = atomic_load_u64(&g_present_stats.row_bytes_written);
    uint64_t failed    = atomic_load_u64(&g_present_stats.failed_attempts);
    uint64_t avg_full  = full_n  ? full_ns  / full_n  : 0;
    uint64_t avg_rect  = rect_n  ? rect_ns  / rect_n  : 0;

    log_debug("gfx_present_stats: full=%llu avg_ns=%llu rect=%llu avg_ns=%llu "
              "bytes=%llu failed=%llu\n",
              full_n, avg_full, rect_n, avg_rect, bytes, failed);
}

// ── gfx_ioctl_present ──────────────────────────────────────
// Spec §4: present validates the request struct's user range,
// snapshots the view rect under the lock, drops the lock, then
// acquires a frame-wide lease and walks the view row-by-row.
// Each row:
//   (a) re-checks the per-row user sub-range with
//       syscall_check_user_range (snapshot, not a pin),
//   (b) fault-tolerant copies the row bytes into a heap row buffer,
//   (c) calls fb_write_row_leased(&lease, x, y + row, buf, stride).
//
// On any fault, kfree the row buffer, release the lease, and return
// -EFAULT — earlier rows may already be visible on the fb (spec §4).
static int gfx_ioctl_present(file_t *f, gfx_present_req_t *ureq)
{
    /* ── Terminal-CPU probe (Phase 1 instrumentation) ───────────────
     * Wall-time cost of a successful present, accumulated and dumped
     * every 256 calls. Counts only the rc==0 success path; error
     * returns are silently excluded so they don't bias the average.
     * Removed once we have the numbers (commit tty-cpu-probe done).
     */
    static uint64_t s_present_ns;
    static uint64_t s_present_count;
    uint64_t t_present0 = arch_clocksource_read_ns();

    gfx_view_t *v = (gfx_view_t *)f->dev_private;
    if (!v || !ureq) return -EINVAL;

    // Stage 1: validate + copy the request struct.
    if (!syscall_check_user_range((uint64_t)ureq,
                                  sizeof(gfx_present_req_t), false))
        return -EFAULT;
    gfx_present_req_t kreq;
    if (copy_from_user_ft(&kreq, ureq, sizeof(kreq)) < 0)
        return -EFAULT;

    // Stage 2: snapshot the view rectangle and mode_seq under the table lock.
    // The lock is dropped before any user-range check or copy_*_ft
    // call (those primitives may longjmp on user fault; holding a
    // spinlock across a longjmp would leak it).
    uint32_t v_x, v_y, v_w, v_h;
    uint64_t v_mode_seq;
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
        v_mode_seq = v->mode_seq;
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

    // Stage 4: acquire lease covering the entire frame.
    fb_lease_t lease;
    int lrc = fb_gfx_writer_begin(&lease, v_mode_seq, v);
    if (lrc < 0) return lrc;

    /* Row-by-row present path. Terminal-cpu investigation (commit
     * tty-cpu-probe-v1) confirmed two redundant costs inside the
     * prior loop:
     *
     *   1. The per-row syscall_check_user_range was redundant —
     *      Stage 3 already range-checked the whole
     *      [pixels, pixels + v_h*stride) user region.
     *   2. The per-row setjmp in copy_from_user_ft was hoistable:
     *      arm current->fault_jmp ONCE for the loop and memcpy
     *      inline per row. A fault on any row longjmps to the
     *      cleanup below; spec §4 already accepts that early rows
     *      may have become visible by then.
     *
     * On QEMU TCG these are near-free because clang folds the
     * setjmp/longjmp pair into IR intrinsic calls and the kernel's
     * TLB is hot, so the per-iteration wall time is dominated by
     * fb_write_row_leased (47.9 µs / row in the recorded run, ≈33 ns
     * per pixel). On bare metal the savings should be visible —
     * each redundant setjmp+PT walk is a few µs, and 1080 of them
     * × 1-2 present/s adds up. See
     * docs/roadmap.md parked "terminal 长时间占用 CPU 约 16%" for
     * the full investigation and follow-up.
     */
    size_t row_buf_size = (size_t)kreq.stride;
    void *row_buf = kmalloc(row_buf_size);
    if (!row_buf) {
        fb_writer_end(&lease);
        return -ENOMEM;
    }

    os01_jmp_buf jb;
    void **old_jmp = current->fault_jmp;
    void (*old_cb)(void *) = current->fault_cleanup;
    void *old_arg = current->fault_cleanup_arg;
    current->fault_jmp = (void **)jb;
    current->fault_cleanup = NULL;
    current->fault_cleanup_arg = NULL;

    int rc;
    if (__builtin_setjmp((void **)jb) == 0) {
        rc = 0;
        for (uint32_t row = 0; row < v_h; row++) {
            uint64_t row_addr = kreq.pixels + (uint64_t)row * (uint64_t)kreq.stride;
            __builtin_memcpy(row_buf, (const void *)(uintptr_t)row_addr,
                             (size_t)kreq.stride);
            int wrc = fb_write_row_leased(&lease, v_x, v_y + row, row_buf,
                                          (uint32_t)kreq.stride);
            if (wrc < 0) { rc = wrc; break; }
        }
    } else {
        rc = -EFAULT;
    }

    current->fault_jmp = old_jmp;
    current->fault_cleanup = old_cb;
    current->fault_cleanup_arg = old_arg;

    kfree(row_buf);
    fb_writer_end(&lease);
    if (rc < 0) return rc;

    /* Fold any pending failed-attempt prefix bytes from a previous
     * rect/full present into the cumulative counter, then add THIS
     * attempt's bytes (v_h * stride for the full path).  Spec §8:
     * kernel gfx counts row-write bytes per success row; the rect
     * path's prefix folding rule applies symmetrically here. */
    fold_prefix_bytes();
    atomic_add_u64(&g_present_stats.row_bytes_written,
                   (uint64_t)v_h * (uint64_t)kreq.stride);

    /* Phase 1 instrumentation: aggregate cost & rate, dump every 256
     * successful presents so a QEMU 10 s run leaves multiple points. */
    {
        uint64_t dt = arch_clocksource_read_ns() - t_present0;
        atomic_add_u64(&g_present_stats.full_success_ns, dt);
        atomic_add_u64(&g_present_stats.full_count, 1);
        s_present_ns += dt;
        s_present_count++;
        if ((s_present_count & 0xffu) == 0) {
            serial_printk("gfx_present: count=%llu total_ns=%llu avg_ns=%llu\n",
                          s_present_count, s_present_ns,
                          s_present_ns / s_present_count);
        }
    }
    stats_log_maybe(arch_clocksource_read_ns());
    return 0;
}

// ── gfx_ioctl_present_rect — Task 1 partial-rectangle path ─────
//
// Spec §4+§5.  Owns one fb_gfx_writer_begin lease across the rect's
// rows, validates geometry/stride/user-range with NO implicit
// clipping, and walks the bounce-per-row path with a single fault
// context (per-prefix byte counter is volatile — see fold rule above).
//
// Error returns: -EFAULT (user range fault / fault mid-memcpy),
// -EINVAL (geometry / stride / reserved / unconfigured view),
// -EACCES / -EAGAIN / -ESTALE / -EIO / -EBUSY / -ENOMEM from the
// lease and kmalloc paths, propagated unchanged.
static int gfx_ioctl_present_rect(file_t *f, gfx_present_rect_req_t *ureq)
{
    uint64_t t_present0 = arch_clocksource_read_ns();

    gfx_view_t *v = (gfx_view_t *)f->dev_private;
    if (!v || !ureq) return -EINVAL;

    // Stage 1: validate + copy the request struct.
    if (!syscall_check_user_range((uint64_t)ureq,
                                  sizeof(gfx_present_rect_req_t), false))
        return -EFAULT;
    gfx_present_rect_req_t kreq;
    if (copy_from_user_ft(&kreq, ureq, sizeof(kreq)) < 0)
        return -EFAULT;

    // Stage 2: snapshot the view rectangle + mode_seq under the lock,
    // then drop the lock BEFORE any user-range check / setjmp arm.
    uint32_t v_x, v_y, v_w, v_h;
    uint64_t v_mode_seq;
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
        v_mode_seq = v->mode_seq;
        spin_unlock_irqrestore(&g_gfx_table.lock, flags);
    }

    // Stage 3: geometry (subtraction form, rejects zero + UINT32_MAX).
    if (kreq.w == 0 || kreq.h == 0)             return -EINVAL;
    if (kreq.x > v_w || kreq.w > v_w - kreq.x)  return -EINVAL;
    if (kreq.y > v_h || kreq.h > v_h - kreq.y)  return -EINVAL;
    if (kreq.reserved != 0)                     return -EINVAL;

    // Spec §4: stride MUST equal view_w * 4.  Reject anything else,
    // including zero stride (would silently underwrite the buffer).
    uint32_t expected_stride = v_w * 4u;
    if (kreq.stride != expected_stride)         return -EINVAL;

    // Stage 4: address arithmetic.  Everything in uint64_t with
    // explicit overflow rejection — the geometry subtractions above
    // do not catch a hostile stride passed as `pixels` (per spec
    // §5 the user pointer must lie in the user range and the
    // envelope must be representable).
    if (kreq.pixels == 0)                       return -EFAULT;
    uint64_t y_term = (uint64_t)kreq.y * (uint64_t)kreq.stride;
    uint64_t x_term = (uint64_t)kreq.x * 4u;
    if (y_term + x_term < y_term)               return -EFAULT;   /* y*stride overflow */
    uint64_t first = kreq.pixels + y_term + x_term;
    if (first < kreq.pixels)                    return -EFAULT;   /* + y*stride overflow */
    /* (h-1)*stride + w*4 */
    uint64_t last_row_off = ((uint64_t)kreq.h - 1u) * (uint64_t)kreq.stride;
    if (last_row_off / (uint64_t)kreq.stride != (uint64_t)kreq.h - 1u) return -EFAULT;
    uint64_t last_row_bytes = last_row_off + (uint64_t)kreq.w * 4u;
    if (last_row_bytes < last_row_off)          return -EFAULT;
    uint64_t length = last_row_bytes;
    if (!syscall_check_user_range(first, length, false))
        return -EFAULT;

    // Stage 5: acquire one lease covering the entire frame.  The
    // lease's snapshot gives us the destination base + mode_seq; we
    // never read live Pos again, so a concurrent mode flip does not
    // tear our writes.
    fb_lease_t lease;
    int lrc = fb_gfx_writer_begin(&lease, v_mode_seq, v);
    if (lrc < 0) return lrc;

    // Stage 6: bounce = exactly w*4 bytes.  Spec §5 rejects
    // allocating anything larger; the per-row copy is always w*4.
    uint32_t row_bytes = kreq.w * 4u;
    void *row_buf = kmalloc(row_bytes);
    if (!row_buf) {
        fb_writer_end(&lease);
        atomic_add_u64(&g_present_stats.failed_attempts, 1);
        return -ENOMEM;
    }

    // Arm the fault context ONCE for the whole loop.  The prefix
    // counter is volatile so the longjmp side still sees a valid
    // store (longjmp is a control-flow transfer, not a memory
    // barrier, and the optimiser must not elide the prefix count).
    os01_jmp_buf jb;
    void **old_jmp = current->fault_jmp;
    void (*old_cb)(void *) = current->fault_cleanup;
    void *old_arg = current->fault_cleanup_arg;
    current->fault_jmp = (void **)jb;
    current->fault_cleanup = NULL;
    current->fault_cleanup_arg = NULL;

    volatile uint64_t prefix_bytes_this_attempt = 0;

    int rc = 0;
    if (__builtin_setjmp((void **)jb) == 0) {
        for (uint32_t i = 0; i < kreq.h; i++) {
            uint64_t row_addr = kreq.pixels +
                                ((uint64_t)kreq.y + (uint64_t)i) *
                                (uint64_t)kreq.stride +
                                (uint64_t)kreq.x * 4u;
            __builtin_memcpy(row_buf, (const void *)(uintptr_t)row_addr,
                             (size_t)row_bytes);
            int wrc = fb_write_row_leased(&lease,
                                          v_x + kreq.x,
                                          v_y + kreq.y + i,
                                          row_buf, row_bytes);
            if (wrc < 0) { rc = wrc; break; }
            prefix_bytes_this_attempt += (uint64_t)row_bytes;
        }
    } else {
        /* Longjmp from inline-memcpy fault.  Spec §5: early rows
         * may have become visible; we count the SUCCESSFUL rows
         * (prefix_bytes_this_attempt) into the prefix-to-fold
         * accumulator and report EFAULT. */
        atomic_add_u64(&g_present_stats.prefix_bytes_to_fold,
                      prefix_bytes_this_attempt);
        rc = -EFAULT;
    }

    current->fault_jmp = old_jmp;
    current->fault_cleanup = old_cb;
    current->fault_cleanup_arg = old_arg;

    kfree(row_buf);
    fb_writer_end(&lease);

    if (rc < 0) {
        atomic_add_u64(&g_present_stats.failed_attempts, 1);
        return rc;
    }

    /* Success path.  Fold any pending failed-prefix bytes BEFORE we
     * add this attempt's bytes — that way the cumulative
     * row_bytes_written is monotonic and the per-second log keeps
     * reading in the same order as the writes happened. */
    fold_prefix_bytes();

    /* `rows_written * row_bytes == prefix_bytes_this_attempt` on the
     * success path; add it directly to the cumulative counter. */
    atomic_add_u64(&g_present_stats.row_bytes_written, prefix_bytes_this_attempt);

    uint64_t dt = arch_clocksource_read_ns() - t_present0;
    atomic_add_u64(&g_present_stats.rect_success_ns, dt);
    atomic_add_u64(&g_present_stats.rect_count, 1);
    stats_log_maybe(arch_clocksource_read_ns());
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
        {
            ssize_t user_copy_rc = copy_to_user_ft(arg, &kinfo, sizeof(kinfo));
            if (user_copy_rc < 0) return user_copy_rc;
        }
        return 0;
    }
    case GFX_PRESENT: {
        // gfx_ioctl_present itself does syscall_check_user_range +
        // copy_from_user_ft on the request struct.
        return gfx_ioctl_present(f, (gfx_present_req_t *)arg);
    }
    case GFX_PRESENT_RECT: {
        // Same staging pattern as GFX_PRESENT — the rect handler
        // itself does syscall_check_user_range + copy_from_user_ft
        // on the 32 B request struct.
        return gfx_ioctl_present_rect(f, (gfx_present_rect_req_t *)arg);
    }
    case GFX_ACQUIRE: {
        gfx_view_t *v = (gfx_view_t *)f->dev_private;
        if (!v) return -EINVAL;
        uint64_t mode_seq;
        {
            uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
            if (!v->configured) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            mode_seq = v->mode_seq;
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        }
        return fb_owner_acquire(v, mode_seq);
    }
    case GFX_RELEASE: {
        gfx_view_t *v = (gfx_view_t *)f->dev_private;
        if (!v) return -EINVAL;
        {
            uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
            if (!v->configured) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        }
        return fb_owner_release(v);
    }
    case GFX_REGISTER_FALLBACK: {
        gfx_view_t *v = (gfx_view_t *)f->dev_private;
        if (!v) return -EINVAL;

        fb_snapshot_t snap;
        int s_rc = fb_snapshot_read(&snap);
        if (s_rc < 0) return s_rc;

        uint64_t mode_seq;
        {
            uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
            if (!v->configured) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            if (v->desc_x != 0 || v->desc_y != 0 ||
                v->desc_w != snap.state.info.width ||
                v->desc_h != snap.state.info.height) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            mode_seq = v->mode_seq;
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        }
        return fb_owner_register_fallback(v, mode_seq);
    }
    case GFX_GET_DISPLAY_STATE: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(gfx_display_state_t), true))
            return -EFAULT;

        gfx_view_t *v = (gfx_view_t *)f->dev_private;
        if (!v) return -EINVAL;
        {
            uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
            if (!v->configured) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        }

        gfx_display_state_t kstate;
        int grc = fb_owner_get_state(v, &kstate);
        if (grc < 0) return grc;

        if (copy_to_user_ft(arg, &kstate, sizeof(kstate)) < 0)
            return -EFAULT;
        return 0;
    }
    case GFX_REPLACE_FALLBACK: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(gfx_replace_fallback_t), false))
            return -EFAULT;

        gfx_replace_fallback_t kreq;
        if (copy_from_user_ft(&kreq, arg, sizeof(kreq)) < 0)
            return -EFAULT;

        if (kreq.reserved != 0)
            return -EINVAL;

        gfx_view_t *new_v = (gfx_view_t *)f->dev_private;
        if (!new_v) return -EINVAL;

        fb_snapshot_t snap;
        int s_rc = fb_snapshot_read(&snap);
        if (s_rc < 0) return s_rc;

        uint64_t new_mode_seq;
        {
            uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
            if (!new_v->configured) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            if (new_v->desc_x != 0 || new_v->desc_y != 0 ||
                new_v->desc_w != snap.state.info.width ||
                new_v->desc_h != snap.state.info.height) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                return -EINVAL;
            }
            new_mode_seq = new_v->mode_seq;
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        }

        if (kreq.old_fd < 0 || kreq.old_fd >= NOFILE) return -EBADF;

        file_t *old_file = files_get_file(current->files, kreq.old_fd);
        if (!old_file) return -EBADF;

        if (old_file->type != FD_DEV) {
            file_put(old_file);
            return -EINVAL;
        }

        const struct devfs_ops *ops = devfs_get_file_ops(old_file);
        if (!old_file->node || ops != &gfx_ops) {
            file_put(old_file);
            return -EINVAL;
        }

        gfx_view_t *old_v = NULL;
        {
            uint64_t flags = spin_lock_irqsave(&g_gfx_table.lock);
            old_v = (gfx_view_t *)old_file->dev_private;
            if (!old_v || !old_v->configured || old_v->slot < 0 ||
                old_v->slot >= GFX_MAX_VIEWS ||
                g_gfx_table.slots[old_v->slot] != old_v) {
                spin_unlock_irqrestore(&g_gfx_table.lock, flags);
                file_put(old_file);
                return -EINVAL;
            }
            spin_unlock_irqrestore(&g_gfx_table.lock, flags);
        }

        int rrc = fb_owner_replace_fallback(old_v, new_v, new_mode_seq);
        file_put(old_file);
        return rrc;
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
