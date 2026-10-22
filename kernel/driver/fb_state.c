/*
 * kernel/driver/fb_state.c
 *
 * Central framebuffer display state coordinator, writer leases, and drain protocol.
 * (Spec §2.1, §3.1, §3.2)
 */

#include <driver/fb_state.h>
#include <driver/fb.h>
#include <driver/bga.h>
#include <driver/fb_test.h>
#include <driver/input_route.h>
#include <core/printk.h>
#include <tty/console.h>
#include <sync/mutex.h>
#include <arch/spinlock.h>
#include <arch/clocksource.h>
#include <sched/task.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* The input route (kernel/driver/input_route.c) is part of the x86 PS/2
 * chain and is therefore x86-selected, while this coordinator belongs
 * to the architecture-neutral driver set.  The references are weak so
 * that set still links on a platform without a PS/2 chain: there the
 * route is absent, no lease can exist, and every bind is refused with
 * -ENODEV. */
#pragma weak input_route_bind
#pragma weak input_route_unbind
#pragma weak input_route_lease_live

/* ── Display coordinator private state ── */
static spinlock_T display_state_lock;
static mutex_t    display_mutex;

static struct fb_state g_fb_state;
static uint64_t        g_fb_phys __attribute__((unused));
static uint64_t        g_fb_gop_bytes __attribute__((unused));
static uint32_t       *g_fb_mapped_addr;
static uint64_t        g_fb_mapped_size;
static uint64_t        g_vram_capacity;
static uint32_t        g_active_writers;
static bool            g_transitioning;
static bool            g_backend_ready;
static bool            g_backend_failed;
static bool            g_raw_mmap_seen;
static bool            g_raw_writable_mmap_seen;
static bool            g_raw_writable_test_allowed;
static bool            g_initialized;

static const void     *g_owner;
static const void     *g_fallback;
static uint64_t        g_owner_generation = 1;

/* ── The physical-input lease (GUI-2 plan Task 4) ────────────────
 * The lease lives with the DISPLAY OWNER: it is granted inside the
 * display control lock, together with the owner and mode check, and it
 * is revoked at every owner / mode state commit point.  The record
 * keeps the WEAK view identity and the lease generation — never a file
 * reference — so the bound raw files can still reach their final
 * close, and that final close still revokes the lease. */
typedef enum {
    FB_INPUT_LEASE_NONE = 0,   /* no lease, no record */
    FB_INPUT_LEASE_LIVE,       /* the route holds the lease */
    FB_INPUT_LEASE_REVOKED,    /* the route lease is gone; the record may be cleaned up */
} fb_input_lease_state_t;

static struct {
    fb_input_lease_state_t state;
    const void            *view;
    uint64_t               generation;
} g_input_lease;

/* Revoke the live lease at an owner / mode state commit point (the
 * display state lock is held). */
static void fb_input_revoke_locked(int reason);

static bga_caps_t      g_bga_caps __attribute__((unused));
static struct fb_info  g_bga_modes[FB_MAX_MODES];
static uint32_t        g_bga_modes_count;

void fb_bootstrap_state(uint64_t phys, uint64_t gop_bytes, const struct fb_info *info)
{
    spin_init(&display_state_lock);
    mutex_init(&display_mutex);

    g_fb_phys = phys;
    g_fb_gop_bytes = gop_bytes;
    if (info) {
        g_fb_state.info = *info;
    } else {
        memset(&g_fb_state.info, 0, sizeof(g_fb_state.info));
    }
    g_fb_state.reserved = 0;
    g_fb_state.generation = 1;

    g_fb_mapped_addr = NULL;
    g_fb_mapped_size = 0;
    g_vram_capacity = gop_bytes;
    g_active_writers = 0;
    g_transitioning = false;
    g_backend_ready = false;
    g_backend_failed = false;
    g_raw_mmap_seen = false;
    g_raw_writable_mmap_seen = false;
    g_raw_writable_test_allowed = false;
    g_initialized = true;

    g_owner = NULL;
    g_fallback = NULL;
    g_owner_generation = 1;

    /* No lease exists before the first bind. */
    g_input_lease.state = FB_INPUT_LEASE_NONE;
    g_input_lease.view = NULL;
    g_input_lease.generation = 0;

    /* Drop any armed fault left over from a previous session (test build
     * only; a no-op in production). */
    fb_test_reset();

    memset(&g_bga_caps, 0, sizeof(g_bga_caps));
    memset(g_bga_modes, 0, sizeof(g_bga_modes));
    g_bga_modes_count = 0;
}

void fb_publish_initial_mapping(uint32_t *addr, uint64_t mapped_size)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_fb_mapped_addr = addr;
    g_fb_mapped_size = mapped_size;
    if (g_vram_capacity < mapped_size) {
        g_vram_capacity = mapped_size;
    }
    spin_unlock_irqrestore(&display_state_lock, flags);
}

int fb_install_backend(const struct bga_caps *caps, uint32_t *addr, uint64_t mapped_size)
{
    if (!caps || !addr || mapped_size == 0 || caps->vram_bytes == 0) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_initialized || g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EIO;
    }

    g_bga_caps = *caps;
    g_fb_mapped_addr = addr;
    g_fb_mapped_size = mapped_size;
    g_vram_capacity = caps->vram_bytes;

    /* Filter modes for published capability */
    g_bga_modes_count = bga_filter_modes(caps, mapped_size, g_bga_modes);

    g_backend_ready = true;
    g_backend_failed = false;
    g_transitioning = false;

    spin_unlock_irqrestore(&display_state_lock, flags);

    /* Update Pos compatibility mirror */
    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    Pos.FB_addr = addr;
    Pos.FB_length = mapped_size;
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    return 0;
}

void fb_control_lock(void)
{
    mutex_lock(&display_mutex);
}

void fb_control_unlock(void)
{
    mutex_unlock(&display_mutex);
}

int fb_snapshot_read(fb_snapshot_t *out)
{
    if (!out) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_initialized) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EIO;
    }

    out->state = g_fb_state;
    out->addr = g_fb_mapped_addr;
    out->mapped_size = g_fb_mapped_size;

    spin_unlock_irqrestore(&display_state_lock, flags);
    return 0;
}

int fb_writer_begin(fb_lease_t *lease, uint64_t expected_generation)
{
    if (!lease) {
        return -EINVAL;
    }
    lease->held = false;

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_initialized) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EIO;
    }
    if (g_transitioning) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (!g_fb_mapped_addr || g_fb_mapped_size == 0) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (g_owner != NULL) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EBUSY;
    }
    if (expected_generation != 0 && expected_generation != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -ESTALE;
    }

    lease->snapshot.state = g_fb_state;
    lease->snapshot.addr = g_fb_mapped_addr;
    lease->snapshot.mapped_size = g_fb_mapped_size;
    lease->held = true;
    g_active_writers++;

    spin_unlock_irqrestore(&display_state_lock, flags);
    return 0;
}

void fb_writer_end(fb_lease_t *lease)
{
    if (!lease || !lease->held) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    lease->held = false;
    if (g_active_writers > 0) {
        g_active_writers--;
    }
    spin_unlock_irqrestore(&display_state_lock, flags);
}

static bool fb_drain_wake_check(struct task_struct *waiter)
{
    return arch_clocksource_read_ns() >= waiter->wakeup_ns;
}

static void fb_drain_pause(uint64_t deadline_ns)
{
    uint64_t now = arch_clocksource_read_ns();
    if (now >= deadline_ns) {
        return;
    }
    uint64_t next = now + 1000000ULL; /* 1 ms */
    if (next > deadline_ns) {
        next = deadline_ns;
    }

    current->wakeup_ns = next;
    do {
        blocker_wait(fb_drain_wake_check, BLOCKER_NANOSLEEP, false);
    } while (arch_clocksource_read_ns() < next);
    current->wakeup_ns = 0;
}

int fb_transition_begin(bool boot_probe)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_initialized) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EIO;
    }
    if (g_transitioning) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EBUSY;
    }

    g_transitioning = true;

    if (boot_probe) {
        if (g_active_writers == 0) {
            spin_unlock_irqrestore(&display_state_lock, flags);
            return 0;
        } else {
            g_transitioning = false;
            spin_unlock_irqrestore(&display_state_lock, flags);
            return -EBUSY;
        }
    }

    /* Runtime transition: check if drain is needed */
    if (g_active_writers == 0) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&display_state_lock, flags);

    uint64_t start_ns = arch_clocksource_read_ns();
    uint64_t deadline_ns = start_ns + 1000000000ULL; /* 1 second */

    while (1) {
        fb_drain_pause(deadline_ns);

        flags = spin_lock_irqsave(&display_state_lock);
        if (g_active_writers == 0) {
            spin_unlock_irqrestore(&display_state_lock, flags);
            return 0;
        }
        uint64_t now_ns = arch_clocksource_read_ns();
        if (now_ns >= deadline_ns) {
            /* Timeout: restore admission */
            g_transitioning = false;
            spin_unlock_irqrestore(&display_state_lock, flags);
            return -EBUSY;
        }
        spin_unlock_irqrestore(&display_state_lock, flags);
    }
}

void fb_transition_end(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_backend_failed) {
        g_transitioning = false;
    }
    spin_unlock_irqrestore(&display_state_lock, flags);
}

void fb_update_console_ownership_locked(void)
{
    if (g_owner == NULL) {
        console_force_enable_locked();
        fb_set_surrendered_locked(false);
    } else {
        console_set_active_locked(false);
        fb_set_surrendered_locked(true);
    }
}

void fb_mark_failed(void)
{
    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_backend_failed = true;
    g_backend_ready = false;
    g_transitioning = true;
    /* The backend is gone: the display ownership — and with it the
     * input lease — is gone too. */
    fb_input_revoke_locked(EIO);
    g_owner = NULL;
    g_fallback = NULL;
    g_owner_generation++;
    fb_update_console_ownership_locked();
    spin_unlock_irqrestore(&display_state_lock, flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    /* A permanent backend fault invalidates every armed fault record
     * (test build only; a no-op in production). */
    fb_test_clear_pending();
}

void fb_mark_raw_mmap_seen(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_raw_mmap_seen = true;
    spin_unlock_irqrestore(&display_state_lock, flags);
}

bool fb_has_raw_mmap_seen(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    bool seen = g_raw_mmap_seen;
    spin_unlock_irqrestore(&display_state_lock, flags);
    return seen;
}

void fb_mark_raw_writable_mmap_seen(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_raw_writable_mmap_seen = true;
    spin_unlock_irqrestore(&display_state_lock, flags);
}

bool fb_has_raw_writable_mmap_seen(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    bool seen = g_raw_writable_mmap_seen;
    spin_unlock_irqrestore(&display_state_lock, flags);
    return seen;
}

bool fb_has_owner_or_fallback(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    bool has = (g_owner != NULL || g_fallback != NULL);
    spin_unlock_irqrestore(&display_state_lock, flags);
    return has;
}

bool fb_is_raw_writable_allowed(void)
{
#if defined(FB_RESOLUTION_TEST) && (FB_RESOLUTION_TEST != 0)
    return true;
#else
    return g_raw_writable_test_allowed;
#endif
}

void fb_set_raw_writable_allowed_for_test(bool allowed)
{
    g_raw_writable_test_allowed = allowed;
}

uint32_t fb_active_writers_count(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    uint32_t count = g_active_writers;
    spin_unlock_irqrestore(&display_state_lock, flags);
    return count;
}

static void fb_commit_layout_locked(const struct fb_info *info, bool redraw_invalidated)
{
    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    uint64_t state_flags = spin_lock_irqsave(&display_state_lock);

    /* The resolution / mode generation is about to move on: every view
     * becomes stale, so the lease that was bound to the old generation
     * is revoked here — at the state commit point, before the old view
     * can act on it any more. */
    if (info || redraw_invalidated) {
        fb_input_revoke_locked(ESTALE);
    }

    if (info) {
        g_fb_state.info = *info;
        Pos.XResolution = (int32_t)info->width;
        Pos.YResolution = (int32_t)info->height;
        if (g_fb_mapped_size > 0) {
            Pos.FB_length = g_fb_mapped_size;
        } else {
            Pos.FB_length = (uint64_t)info->width * info->height * 4;
        }
    }

    if (info || redraw_invalidated) {
        g_fb_state.generation++;
    }

    console_notify_resize_locked();

    spin_unlock_irqrestore(&display_state_lock, state_flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);
}

int fb_get_state(struct fb_state *out)
{
    if (!out) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    *out = g_fb_state;
    out->reserved = 0;
    spin_unlock_irqrestore(&display_state_lock, flags);

    fb_control_unlock();
    return 0;
}

int fb_get_modes(uint32_t capacity, struct fb_modes_req *out)
{
    if (!out) {
        return -EINVAL;
    }
    if (capacity > FB_MAX_MODES) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }
    if (!g_backend_ready) {
        fb_control_unlock();
        return -ENODEV;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    memset(out, 0, sizeof(*out));
    uint32_t total = g_bga_modes_count;
    uint32_t count = capacity < total ? capacity : total;
    out->capacity = capacity;
    out->count = count;
    out->total = total;
    for (uint32_t i = 0; i < count; i++) {
        out->modes[i] = g_bga_modes[i];
    }
    spin_unlock_irqrestore(&display_state_lock, flags);

    fb_control_unlock();
    return 0;
}

int fb_set_mode(const struct fb_set_mode_req *req)
{
    if (!req) {
        return -EINVAL;
    }

    uint32_t bpp = req->bpp;
    if (bpp != 0 && bpp != 32) {
        return -EINVAL;
    }
    if (req->width == 0 || req->height == 0) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }
    if (!g_backend_ready) {
        fb_control_unlock();
        return -ENODEV;
    }

    /* Check whitelist */
    bool found = false;
    for (uint32_t i = 0; i < g_bga_modes_count; i++) {
        if (g_bga_modes[i].width == req->width &&
            g_bga_modes[i].height == req->height) {
            found = true;
            break;
        }
    }
    if (!found) {
        fb_control_unlock();
        return -EINVAL;
    }

    /* Capacity check */
    uint64_t target_size = (uint64_t)req->width * req->height * 4;
    uint64_t limit = g_vram_capacity < g_fb_mapped_size ? g_vram_capacity : g_fb_mapped_size;
    if (target_size > limit) {
        fb_control_unlock();
        return -EINVAL;
    }

    /* No-op check: identical mode returns 0 immediately */
    if (g_fb_state.info.width == req->width &&
        g_fb_state.info.height == req->height &&
        g_fb_state.info.bpp == 32) {
        fb_control_unlock();
        return 0;
    }

    /* Raw sticky check: changing mode when mmap seen returns -EBUSY */
    if (g_raw_mmap_seen) {
        fb_control_unlock();
        return -EBUSY;
    }

    /* Writer admission drain */
    int tr_rc = fb_transition_begin(false);
    if (tr_rc < 0) {
        fb_control_unlock();
        return tr_rc;
    }

    uint64_t old_active_size = (uint64_t)g_fb_state.info.width * g_fb_state.info.height * 4;
    struct fb_info target_info = {
        .width = req->width,
        .height = req->height,
        .stride = req->width * 4,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };

    /* Register the pending fault association for the calling PID; only a
     * layout-changing SET consumes an armed register-readback fault, and
     * only when the target PID matches.  No-op in production. */
    fb_test_set_begin((int32_t)current->pid);
    enum bga_result bres = bga_apply_mode(&target_info);
    fb_test_set_end();

    if (bres == BGA_APPLIED) {
        if (Pos.FB_addr) {
            memset(Pos.FB_addr, 0, (uint64_t)req->width * req->height * 4);
        }
        fb_commit_layout_locked(&target_info, false);
        fb_transition_end();
        fb_control_unlock();
        return 0;
    } else if (bres == BGA_ROLLED_BACK) {
        if (Pos.FB_addr && old_active_size > 0) {
            memset(Pos.FB_addr, 0, old_active_size);
        }
        fb_commit_layout_locked(NULL, true);
        fb_transition_end();
        fb_control_unlock();
        return -EIO;
    } else {
        /* BGA_FAILED: permanently close admission */
        fb_mark_failed();
        fb_control_unlock();
        return -EIO;
    }
}

#ifdef OS01_HOST_TEST
void fb_state__test_set_generation(uint64_t generation)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_fb_state.generation = generation;
    spin_unlock_irqrestore(&display_state_lock, flags);
}
#endif

int fb_gfx_writer_begin(fb_lease_t *lease, uint64_t mode_seq, const void *cookie)
{
    if (!lease) {
        return -EINVAL;
    }
    lease->held = false;

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_initialized) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EIO;
    }
    if (g_transitioning) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (!g_fb_mapped_addr || g_fb_mapped_size == 0) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (cookie == NULL || g_owner != cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EACCES;
    }
    if (mode_seq != 0 && mode_seq != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -ESTALE;
    }

    lease->snapshot.state = g_fb_state;
    lease->snapshot.addr = g_fb_mapped_addr;
    lease->snapshot.mapped_size = g_fb_mapped_size;
    lease->held = true;
    g_active_writers++;

    spin_unlock_irqrestore(&display_state_lock, flags);
    return 0;
}

int fb_owner_get_state(const void *cookie, gfx_display_state_t *out)
{
    if (!out) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (!g_initialized) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EAGAIN;
    }
    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        return -EIO;
    }

    uint32_t role_flags = 0;
    if (cookie != NULL) {
        if (g_owner == cookie) {
            role_flags |= GFX_DISPLAY_STATE_FOREGROUND;
        }
        if (g_fallback == cookie) {
            role_flags |= GFX_DISPLAY_STATE_FALLBACK;
        }
    }

    out->generation = g_owner_generation;
    out->flags = role_flags;
    out->reserved = 0;

    spin_unlock_irqrestore(&display_state_lock, flags);
    return 0;
}

/* ── The physical-input lease ─────────────────────────────────────
 * Revoke the live lease at an owner / mode state commit point.  The
 * display state lock is held (and the display mutex on the owner
 * paths): the lock order is display_mutex → display_state_lock →
 * input_route, and the route never takes a display lock or the
 * display mutex, so this cannot invert.
 *
 * The CAPTURED generation is handed to the route: a revoke therefore
 * never takes away a NEWER lease of the same view.  The record
 * survives the revoke — a revoked record may still be cleaned up by
 * its own view. */
static void fb_input_revoke_locked(int reason)
{
    if (g_input_lease.state != FB_INPUT_LEASE_LIVE)
        return;

    const void *view = g_input_lease.view;
    uint64_t generation = g_input_lease.generation;

    g_input_lease.state = FB_INPUT_LEASE_REVOKED;

    if (input_route_unbind)
        input_route_unbind(view, generation, reason);
}

/* Does a live lease block a new bind?
 *
 * The route revokes the lease on its own — the final close of a bound
 * raw file revokes it — and that revoke happens INSIDE the route, so
 * the coordinator's own record goes stale: it would answer EBUSY for
 * a lease that no longer exists.  The record is therefore reconciled
 * with the route before the answer is given, so the two components
 * never disagree about "is there a lease?".
 *
 * The query takes only the route lock (never a display lock) and runs
 * in the coordinator's own context, never in the IRQ path: the lock
 * order stays display_mutex → display_state_lock → route.
 *
 * The route's exclusivity is PER LEASED DEVICE (a keyboard-only lease
 * does not own the mouse), while the coordinator refuses a second
 * bind outright.  For every sequence the UAPI can express the two
 * agree — the keyboard is mandatory, so a second bind always collides
 * on the keyboard — and the coordinator stays the stricter of the two
 * on purpose: a mouse-only lease must never break that invariant. */
static bool fb_input_lease_blocks_locked(void)
{
    if (g_input_lease.state != FB_INPUT_LEASE_LIVE) {
        return false;
    }

    const void *view = NULL;
    uint64_t generation = 0;

    if (input_route_lease_live && input_route_lease_live(&view, &generation)) {
        /* The route holds a lease: ours (the same view and
         * generation) or one that is not ours.  Neither may be
         * preempted. */
        return true;
    }

    /* The route holds no lease any more: the record was stale (the
     * application closed a bound raw file, which revokes the lease
     * inside the route).  Drop it and let the bind proceed. */
    g_input_lease.state = FB_INPUT_LEASE_NONE;
    g_input_lease.view = NULL;
    g_input_lease.generation = 0;
    return false;
}

int fb_input_bind(const void *view, uint64_t expected_mode,
                  struct file *kbd, struct file *mouse,
                  uint64_t *generation, uint32_t *initial_modifiers)
{
    if (!view || !kbd) {
        return -EINVAL;
    }
    if (!input_route_bind) {
        /* No input route in this build (no PS/2 chain). */
        return -ENODEV;
    }

    fb_control_lock();

    uint64_t flags = spin_lock_irqsave(&display_state_lock);

    int rc = 0;
    uint64_t gen = 0;
    uint32_t mods = 0;

    if (!g_initialized) {
        rc = -EAGAIN;
    } else if (g_backend_failed) {
        rc = -EIO;
    } else if (g_owner != view) {
        /* Only the display owner may hold the physical input. */
        rc = -EACCES;
    } else if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        /* The view is stale: the display moved on since it was made. */
        rc = -ESTALE;
    } else if (fb_input_lease_blocks_locked()) {
        /* A lease already exists: the bind never preempts it. */
        rc = -EBUSY;
    } else {
        /* The owner check and the lease commit happen in the SAME
         * display critical section, so a concurrent owner change can
         * never slip between them. */
        rc = input_route_bind(view, kbd, mouse, &gen, &mods);
        if (rc == 0) {
            g_input_lease.state = FB_INPUT_LEASE_LIVE;
            g_input_lease.view = view;
            g_input_lease.generation = gen;
            if (generation) {
                *generation = gen;
            }
            if (initial_modifiers) {
                /* The UAPI publishes the Caps latch ALONE: the route's
                 * aggregate Shift/Ctrl/Alt bits never cross the boundary
                 * (the keys that are still held at bind time are isolated
                 * and their release is swallowed, so those bits would
                 * describe presses the consumer never saw).  The Caps
                 * latch is a STATE, not an edge, and the consumer has no
                 * other way to learn it. */
                *initial_modifiers = (mods & INPUT_ROUTE_MOD_CAPS) ? 1u : 0u;
            }
        }
    }

    spin_unlock_irqrestore(&display_state_lock, flags);
    fb_control_unlock();
    return rc;
}

void fb_input_unbind(const void *view, uint64_t generation, int reason)
{
    if (!view) {
        return;
    }

    fb_control_lock();

    uint64_t flags = spin_lock_irqsave(&display_state_lock);

    /* Only THIS view's lease may be released: another view's lease is
     * never touched, and a view without a lease releases nothing. */
    if (g_input_lease.state != FB_INPUT_LEASE_NONE &&
        g_input_lease.view == view &&
        (generation == 0 || generation == g_input_lease.generation)) {
        uint64_t lease_generation = g_input_lease.generation;
        g_input_lease.state = FB_INPUT_LEASE_NONE;
        g_input_lease.view = NULL;
        g_input_lease.generation = 0;

        if (input_route_unbind) {
            /* The captured generation is handed to the route: it matches
             * the live lease exactly, so an old cleanup never releases a
             * newer lease of the same view. */
            input_route_unbind(view, generation ? generation : lease_generation,
                               reason);
        }
    }

    spin_unlock_irqrestore(&display_state_lock, flags);
    fb_control_unlock();
}

int fb_owner_register_fallback(const void *cookie, uint64_t expected_mode)
{
    if (!cookie) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (g_raw_writable_mmap_seen) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -EBUSY;
    }
    if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -ESTALE;
    }
    if (g_fallback != NULL && g_fallback != cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -EBUSY;
    }
    if (g_fallback == cookie && (g_owner == cookie || g_owner != NULL)) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return 0;
    }
    bool will_become_owner = (g_owner == NULL);
    spin_unlock_irqrestore(&display_state_lock, flags);

    if (will_become_owner) {
        int tr_rc = fb_transition_begin(false);
        if (tr_rc < 0) {
            fb_control_unlock();
            return tr_rc;
        }
    }

    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    flags = spin_lock_irqsave(&display_state_lock);

    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (will_become_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -EIO;
    }

    if (g_raw_writable_mmap_seen) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (will_become_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -EBUSY;
    }

    if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (will_become_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -ESTALE;
    }

    if (g_fallback != NULL && g_fallback != cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (will_become_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -EBUSY;
    }

    bool role_changed = false;
    if (g_fallback != cookie) {
        g_fallback = cookie;
        role_changed = true;
    }
    if (g_owner == NULL) {
        g_owner = cookie;
        role_changed = true;
    }
    if (role_changed) {
        g_owner_generation++;
        fb_update_console_ownership_locked();
    }

    spin_unlock_irqrestore(&display_state_lock, flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    if (will_become_owner) {
        fb_transition_end();
    }
    fb_control_unlock();
    return 0;
}

int fb_owner_acquire(const void *cookie, uint64_t expected_mode)
{
    if (!cookie) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (g_raw_writable_mmap_seen) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -EBUSY;
    }
    if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -ESTALE;
    }
    if (g_owner == cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return 0;
    }
    if (g_owner != NULL && g_owner != g_fallback) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -EBUSY;
    }
    spin_unlock_irqrestore(&display_state_lock, flags);

    int tr_rc = fb_transition_begin(false);
    if (tr_rc < 0) {
        fb_control_unlock();
        return tr_rc;
    }

    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    flags = spin_lock_irqsave(&display_state_lock);

    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        fb_transition_end();
        fb_control_unlock();
        return -EIO;
    }

    if (g_raw_writable_mmap_seen) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        fb_transition_end();
        fb_control_unlock();
        return -EBUSY;
    }

    if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        fb_transition_end();
        fb_control_unlock();
        return -ESTALE;
    }

    if (g_owner != NULL && g_owner != g_fallback && g_owner != cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        fb_transition_end();
        fb_control_unlock();
        return -EBUSY;
    }

    if (g_owner != cookie) {
        /* The owner changes: the lease that lived with the old owner
         * is revoked here, at the commit point, before the old view can
         * be freed. */
        fb_input_revoke_locked(EACCES);
        g_owner = cookie;
        g_owner_generation++;
        fb_update_console_ownership_locked();
    }

    spin_unlock_irqrestore(&display_state_lock, flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    fb_transition_end();
    fb_control_unlock();
    return 0;
}

int fb_owner_release(const void *cookie)
{
    if (!cookie) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (g_owner != cookie || g_owner == g_fallback) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return 0;
    }
    spin_unlock_irqrestore(&display_state_lock, flags);

    int tr_rc = fb_transition_begin(false);
    if (tr_rc < 0) {
        fb_control_unlock();
        return tr_rc;
    }

    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    flags = spin_lock_irqsave(&display_state_lock);

    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        fb_transition_end();
        fb_control_unlock();
        return -EIO;
    }

    if (g_owner == cookie && g_owner != g_fallback) {
        /* The view gives the display up: its input lease goes with it. */
        fb_input_revoke_locked(EACCES);
        g_owner = g_fallback;
        g_owner_generation++;
        fb_update_console_ownership_locked();
    }

    spin_unlock_irqrestore(&display_state_lock, flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    fb_transition_end();
    fb_control_unlock();
    return 0;
}

int fb_owner_replace_fallback(const void *old_cookie, const void *new_cookie, uint64_t expected_mode)
{
    if (!old_cookie || !new_cookie) {
        return -EINVAL;
    }

    fb_control_lock();

    if (!g_initialized) {
        fb_control_unlock();
        return -EAGAIN;
    }
    if (g_backend_failed) {
        fb_control_unlock();
        return -EIO;
    }

    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -ESTALE;
    }
    if (g_fallback != old_cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return -EINVAL;
    }
    if (old_cookie == new_cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        fb_control_unlock();
        return 0;
    }
    bool old_was_owner = (g_owner == old_cookie);
    spin_unlock_irqrestore(&display_state_lock, flags);

    if (old_was_owner) {
        int tr_rc = fb_transition_begin(false);
        if (tr_rc < 0) {
            fb_control_unlock();
            return tr_rc;
        }
    }

    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    flags = spin_lock_irqsave(&display_state_lock);

    if (g_backend_failed) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (old_was_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -EIO;
    }

    if (expected_mode != 0 && expected_mode != g_fb_state.generation) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (old_was_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -ESTALE;
    }

    if (g_fallback != old_cookie) {
        spin_unlock_irqrestore(&display_state_lock, flags);
        spin_unlock_irqrestore(&Pos.lock, pos_flags);
        if (old_was_owner) {
            fb_transition_end();
        }
        fb_control_unlock();
        return -EINVAL;
    }

    g_fallback = new_cookie;
    if (g_owner == old_cookie) {
        /* The fallback handover really changes the owner: the old
         * owner's lease is revoked.  A handover that leaves the owner
         * alone revokes nothing. */
        fb_input_revoke_locked(EACCES);
        g_owner = new_cookie;
    }
    g_owner_generation++;
    fb_update_console_ownership_locked();

    spin_unlock_irqrestore(&display_state_lock, flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    if (old_was_owner) {
        fb_transition_end();
    }
    fb_control_unlock();
    return 0;
}

void fb_owner_detach(const void *cookie)
{
    if (!cookie) {
        return;
    }

    fb_control_lock();

    uint64_t pos_flags = spin_lock_irqsave(&Pos.lock);
    uint64_t flags = spin_lock_irqsave(&display_state_lock);

    bool changed = false;

    if (g_fallback == cookie) {
        g_fallback = NULL;
        changed = true;
    }

    if (g_owner == cookie) {
        /* The view goes away: its input lease must die with it, and it
         * must die BEFORE the view memory is freed. */
        fb_input_revoke_locked(EACCES);
        if (g_fallback != NULL) {
            g_owner = g_fallback;
        } else {
            g_owner = NULL;
        }
        changed = true;
    }

    if (changed) {
        g_owner_generation++;
        fb_update_console_ownership_locked();
    }

    spin_unlock_irqrestore(&display_state_lock, flags);
    spin_unlock_irqrestore(&Pos.lock, pos_flags);

    fb_control_unlock();
}

