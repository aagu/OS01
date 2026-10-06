/*
 * kernel/driver/fb_state.c
 *
 * Central framebuffer display state coordinator, writer leases, and drain protocol.
 * (Spec §2.1, §3.1, §3.2)
 */

#include <driver/fb_state.h>
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
static bool            g_backend_ready __attribute__((unused));
static bool            g_backend_failed;
static bool            g_raw_mmap_seen __attribute__((unused));
static bool            g_initialized;

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
}

#ifdef OS01_HOST_TEST
void fb_state__test_set_generation(uint64_t generation)
{
    uint64_t flags = spin_lock_irqsave(&display_state_lock);
    g_fb_state.generation = generation;
    spin_unlock_irqrestore(&display_state_lock, flags);
}
#endif
