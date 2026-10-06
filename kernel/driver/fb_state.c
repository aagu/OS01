/*
 * kernel/driver/fb_state.c
 *
 * Central framebuffer display state coordinator, writer leases, and drain protocol.
 * (Spec §2.1, §3.1, §3.2)
 */

#include <driver/fb_state.h>
#include <driver/bga.h>
#include <driver/fb_test.h>
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
static bool            g_initialized;

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
    g_initialized = true;

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

void fb_mark_failed(void)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_backend_failed = true;
    g_backend_ready = false;
    g_transitioning = true;
    spin_unlock_irqrestore(&display_state_lock, flags);

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
