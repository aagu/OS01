/*
 * input_route.c — the exclusive physical-input lease
 *
 * The route sits between the PS/2 drivers and every reader of the
 * keyboard / mouse bytes and owns three things:
 *
 *   1. the PHYSICAL key state and the Caps Lock latch — the single
 *      authority, so the Caps state is never flipped twice and the
 *      auto-repeat of a held key never flips it again;
 *   2. the LEASE — one live lease at a time, owned by one display
 *      view.  While it is live the owner's file reads the ACTIVE
 *      queue and every other file on the device is shut out with
 *      EBUSY / POLLERR, so no other reader can steal the bytes and
 *      the keyboard stops leaking into the console TTY;
 *   3. the ROUTING decision — a COMPLETE scan sequence (never half of
 *      one) is published either to the legacy NORMAL path or to the
 *      leased ACTIVE path.
 *
 * Locking
 * ───────
 * The route's own lock (route.lock) protects the lease, the physical
 * and isolation bitmaps, the framer, the ACTIVE queues and the
 * endpoint records.  The lock order is route → NORMAL queue/poll: the
 * route calls the NORMAL sink while it holds route.lock, and the
 * sink's own lock is therefore always taken INSIDE the route lock.
 *
 * The route never takes the i8042 lock, the display state lock or the
 * display mutex.  The mouse driver calls input_route_mouse_event()
 * BEFORE it takes its own event lock, so the forbidden inversion
 * (event lock → route lock) cannot happen.
 *
 * The NORMAL read/poll authorises, dequeues and registers its waiter
 * inside the route's own critical section: authorising and then
 * unlocking and reading a stale ring is forbidden, and a publish that
 * races the waiter registration must not be lost.
 *
 * The framer contract
 * ───────────────────
 * input_ps2_frame_feed() uses the caller-supplied frame as the
 * SEQUENCE ACCUMULATOR and guards it with a per-sequence token plus a
 * byte checksum.  The accumulator therefore has to be the SAME object
 * with a STABLE ADDRESS for every byte of one sequence: it is the
 * `frame` field of the route state, never a per-byte stack local.  The
 * route state is zero-initialised (input_ps2_frame_reset() does NOT
 * clear the token), so a stale token can never reject the first
 * sequence.
 *
 * A -EINVAL from feed() means the sequence is LOST: no physical state
 * update, no published frame, no resumption.  A -EILSEQ means the byte
 * stream is corrupt: the offending byte is consumed, the partial
 * sequence is dropped and nothing is published.
 */

#include <driver/input_route.h>
#include <driver/input_ps2.h>

#include <fs/file.h>
#include <fs/poll.h>
#include <memory/slab.h>
#include <arch/spinlock.h>
#include <kernel.h>
#include <errno.h>
#include <string.h>
#include <log/log.h>

/* ── Sizes ─────────────────────────────────────────────────────── */

/* The ACTIVE keyboard queue is a byte ring with one slot reserved, so
 * it holds 255 usable bytes of raw Set 1 payload. */
#define ROUTE_KBD_QUEUE_BYTES   256
#define ROUTE_KBD_QUEUE_USABLE  (ROUTE_KBD_QUEUE_BYTES - 1)

/* The ACTIVE mouse queue holds 64 whole events. */
#define ROUTE_MOUSE_QUEUE_SLOTS 64

/* The physical key bitmap covers the bare Set 1 codes (0x00-0xFF) and
 * the E0-prefixed ones (0x100|code). */
#define ROUTE_KEY_SLOTS 512

/* Set 1 codes the route reasons about. */
#define KBD_SHIFT_L 0x2A
#define KBD_SHIFT_R 0x36
#define KBD_CTRL_L  0x1D
#define KBD_CTRL_R  (INPUT_PS2_KEY_E0 | 0x1D)
#define KBD_ALT_L   0x38
#define KBD_ALT_R   (INPUT_PS2_KEY_E0 | 0x38)
#define KBD_CAPS    0x3A

/* ── The per-file endpoint (private to this file) ─────────────── */

struct route_endpoint {
    input_device_t device;   /* which device this file belongs to */
    uint64_t      generation;/* the lease generation it joined (0 = never) */
    int           error;     /* 0 = healthy, else the revoke reason */
    int           revoked;   /* the owning lease was taken away */
};

/* ── The route state ──────────────────────────────────────────── */

static struct {
    spinlock_T lock;

    /* The lease.  The view and the endpoint pointers are WEAK: the
     * route never holds a reference that could keep the final close
     * from ever firing, and it never dereferences a view it does not
     * own. */
    int                  lease_active;
    const void          *view;
    uint64_t             generation;      /* the live lease generation */
    uint64_t             next_generation; /* the last generation handed out */
    uint64_t             epoch;           /* bumped on every lease change */
    struct route_endpoint *kbd_ep;
    struct route_endpoint *mouse_ep;

    /* The physical state.  It survives a lease change: a key that is
     * really held is still held when the lease ends. */
    uint8_t  phys_down[ROUTE_KEY_SLOTS];
    uint8_t  isolated[ROUTE_KEY_SLOTS]; /* the keys whose events are swallowed */
    uint8_t  caps_on;                    /* the Caps Lock latch */
    uint32_t logical_mods;               /* the transient modifier state */

    /* The framer.  `frame` is the SEQUENCE ACCUMULATOR: it must keep a
     * stable address for every byte of one sequence. */
    input_ps2_frame_state_t framer;
    input_ps2_frame_t       frame;

    /* The ACTIVE queues. */
    uint8_t       kbd_queue[ROUTE_KBD_QUEUE_BYTES];
    unsigned      kbd_head, kbd_tail, kbd_count;
    int           kbd_overflow;
    /* The DESYNC flag: a sequence crossed an epoch boundary, so the
     * consumer's key state may have diverged from the physical one.
     * It is per-lease (cleared at bind) and sticky until the consumer
     * is told. */
    int           kbd_desync;
    mouse_event_t mouse_queue[ROUTE_MOUSE_QUEUE_SLOTS];
    unsigned      mouse_head, mouse_tail, mouse_count;
    int           mouse_overflow;

    /* The mouse buttons: the physical state and the ones isolated
     * because they were already held when the lease started. */
    uint8_t mouse_buttons;
    uint8_t mouse_isolated;

    /* The poll waiters of the ACTIVE queues. */
    list_t poll;

    /* The NORMAL-path sinks the drivers register. */
    const input_route_kbd_sink_t   *kbd_sink;
    const input_route_mouse_sink_t *mouse_sink;
} route;

/* ── Small helpers ────────────────────────────────────────────── */

static uint32_t route_modifier_bit(uint16_t key)
{
    switch (key) {
    case KBD_SHIFT_L:
    case KBD_SHIFT_R:
        return INPUT_ROUTE_MOD_SHIFT;
    case KBD_CTRL_L:
    case KBD_CTRL_R:
        return INPUT_ROUTE_MOD_CTRL;
    case KBD_ALT_L:
    case KBD_ALT_R:
        return INPUT_ROUTE_MOD_ALT;
    default:
        return 0;
    }
}

/* The modifiers that are physically held right now.  Used for the
 * snapshot a new lease starts from. */
static uint32_t route_physical_modifiers_locked(void)
{
    uint32_t mods = 0;

    if (route.phys_down[KBD_SHIFT_L] || route.phys_down[KBD_SHIFT_R])
        mods |= INPUT_ROUTE_MOD_SHIFT;
    if (route.phys_down[KBD_CTRL_L] || route.phys_down[KBD_CTRL_R])
        mods |= INPUT_ROUTE_MOD_CTRL;
    if (route.phys_down[KBD_ALT_L] || route.phys_down[KBD_ALT_R])
        mods |= INPUT_ROUTE_MOD_ALT;
    if (route.caps_on)
        mods |= INPUT_ROUTE_MOD_CAPS;
    return mods;
}

/* The logical modifier state the NORMAL translator sees: the
 * transient modifiers plus the Caps latch. */
static uint32_t route_logical_snapshot_locked(void)
{
    uint32_t mods = route.logical_mods;

    if (route.caps_on)
        mods |= INPUT_ROUTE_MOD_CAPS;
    return mods;
}

static int route_endpoint_is_owner_locked(const struct route_endpoint *ep)
{
    if (!route.lease_active)
        return 0;
    if (ep == route.kbd_ep || ep == route.mouse_ep)
        return 1;
    return 0;
}

/* The lease is exclusive PER LEASED DEVICE: a keyboard-only lease does
 * not own the mouse, so the mouse stays on the NORMAL path and every
 * reader may read it. */
static int route_device_is_leased_locked(const struct route_endpoint *ep)
{
    if (!route.lease_active)
        return 0;
    if (ep->device == INPUT_DEVICE_KEYBOARD)
        return route.kbd_ep != NULL;
    return route.mouse_ep != NULL;
}

/* ── The ACTIVE queues ────────────────────────────────────────── */

static void route_kbd_queue_reset_locked(void)
{
    route.kbd_head = route.kbd_tail = route.kbd_count = 0;
    route.kbd_overflow = 0;
    route.kbd_desync = 0;
}

static void route_mouse_queue_reset_locked(void)
{
    route.mouse_head = route.mouse_tail = route.mouse_count = 0;
    route.mouse_overflow = 0;
}

/* Wake every waiter registered on the ACTIVE queues.  The route lock
 * is held, so the waiters cannot be freed underneath us. */
static void route_wake_pollers_locked(void)
{
    while (!list_is_empty(&route.poll)) {
        list_t *node = route.poll.next;
        poll_wait_entry_t *entry;

        list_del_init(node);
        entry = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(entry->poll_wq);
    }
}

/* Publish one COMPLETE sequence into the ACTIVE keyboard queue.  The
 * sequence is rejected as a whole when it does not fit: a truncated
 * sequence would be a half sequence at the consumer. */
static void route_kbd_push_locked(const uint8_t *bytes, uint8_t len)
{
    uint8_t i;

    if (!len)
        return;
    if (route.kbd_count + len > ROUTE_KBD_QUEUE_USABLE) {
        log_warn("input: ACTIVE keyboard queue overflow (%u bytes, %u needed)\n",
                 route.kbd_count, (unsigned)len);
        route.kbd_overflow = 1;
        return;
    }
    for (i = 0; i < len; i++) {
        route.kbd_queue[route.kbd_head] = bytes[i];
        route.kbd_head = (route.kbd_head + 1) % ROUTE_KBD_QUEUE_BYTES;
        route.kbd_count++;
    }
    route_wake_pollers_locked();
}

static void route_mouse_push_locked(const mouse_event_t *event)
{
    if (route.mouse_count >= ROUTE_MOUSE_QUEUE_SLOTS) {
        log_warn("input: ACTIVE mouse queue overflow (%u events)\n",
                 route.mouse_count);
        route.mouse_overflow = 1;
        return;
    }
    route.mouse_queue[route.mouse_head] = *event;
    route.mouse_head = (route.mouse_head + 1) % ROUTE_MOUSE_QUEUE_SLOTS;
    route.mouse_count++;
    route_wake_pollers_locked();
}

static int route_active_read_locked(const struct route_endpoint *ep,
                                    uint64_t size, void *buf)
{
    unsigned n, i;

    if (ep->device == INPUT_DEVICE_KEYBOARD) {
        if (route.kbd_desync) {
            /* The consumer's key state may have diverged: a sequence was
             * swallowed at an epoch boundary.  The flag is sticky until
             * the consumer is told — telling it clears the flag, and the
             * byte stream itself stays intact, so the consumer can keep
             * reading after it has re-synced its key state.
             *
             * -ESTALE is reported here; a DEAD endpoint (a revoked
             * lease) reports its recorded reason instead and stays dead
             * for good, so the two are told apart by the retry. */
            route.kbd_desync = 0;
            return -ESTALE;
        }
        if (route.kbd_overflow) {
            /* The ACTIVE stream is untrustworthy: the error is sticky
             * for the life of the lease, so the read AND the poll keep
             * reporting it.  The consumer has to re-acquire the lease. */
            return -EOVERFLOW;
        }
        if (!route.kbd_count)
            return -EAGAIN;
        n = (size < route.kbd_count) ? (unsigned)size : route.kbd_count;
        for (i = 0; i < n; i++) {
            ((uint8_t *)buf)[i] = route.kbd_queue[route.kbd_tail];
            route.kbd_tail = (route.kbd_tail + 1) % ROUTE_KBD_QUEUE_BYTES;
        }
        route.kbd_count -= n;
        return (int)n;
    }

    if (route.mouse_overflow) {
        /* Sticky, like the keyboard overflow above. */
        return -EOVERFLOW;
    }
    if (!route.mouse_count)
        return -EAGAIN;
    if (size < sizeof(mouse_event_t))
        return -EINVAL;
    n = (unsigned)(size / sizeof(mouse_event_t));
    if (n > route.mouse_count)
        n = route.mouse_count;
    for (i = 0; i < n; i++) {
        ((mouse_event_t *)buf)[i] = route.mouse_queue[route.mouse_tail];
        route.mouse_tail = (route.mouse_tail + 1) % ROUTE_MOUSE_QUEUE_SLOTS;
    }
    route.mouse_count -= n;
    return (int)(n * sizeof(mouse_event_t));
}

/* ── The NORMAL path ──────────────────────────────────────────── */

static int route_normal_read_locked(const struct route_endpoint *ep,
                                    uint64_t offset, uint64_t size, void *buf)
{
    if (ep->device == INPUT_DEVICE_KEYBOARD) {
        if (route.kbd_sink && route.kbd_sink->read)
            return route.kbd_sink->read(offset, size, buf);
    } else {
        if (route.mouse_sink && route.mouse_sink->read)
            return route.mouse_sink->read(offset, size, buf);
    }
    return -ENODEV;
}

static uint32_t route_normal_poll_locked(const struct route_endpoint *ep,
                                          uint32_t requested,
                                          struct poll_table *pt)
{
    if (ep->device == INPUT_DEVICE_KEYBOARD) {
        if (route.kbd_sink && route.kbd_sink->poll)
            return route.kbd_sink->poll(requested, pt);
    } else {
        if (route.mouse_sink && route.mouse_sink->poll)
            return route.mouse_sink->poll(requested, pt);
    }
    return 0;
}

/* ── The lease ────────────────────────────────────────────────── */

/* Revoke the live lease.  The route lock is held.
 *
 * `reason` is 0 for an explicit cleanup (the endpoint falls back to
 * the NORMAL path) and a revoke reason (EACCES / ESTALE / EIO) when
 * the lease is taken away (the endpoint stays dead and reports the
 * reason). */
static void route_revoke_locked(int reason)
{
    if (!route.lease_active)
        return;

    /* The endpoint records keep their generation and their error. */
    if (route.kbd_ep) {
        route.kbd_ep->generation = route.generation;
        if (reason) {
            route.kbd_ep->revoked = 1;
            route.kbd_ep->error = reason;
        }
    }
    if (route.mouse_ep) {
        route.mouse_ep->generation = route.generation;
        if (reason) {
            route.mouse_ep->revoked = 1;
            route.mouse_ep->error = reason;
        }
    }

    route.lease_active = 0;
    route.view = NULL;          /* never dereference a freed view */
    route.kbd_ep = NULL;
    route.mouse_ep = NULL;

    /* The transient modifier state is reset: the shell must not inherit
     * the modifier keys the GUI session held.  The PHYSICAL state is
     * NOT reset: the keys are still held. */
    route.logical_mods = 0;

    /* The next lease is a new epoch. */
    route.epoch++;

    route_kbd_queue_reset_locked();
    route_mouse_queue_reset_locked();

    /* The waiters must see the new state. */
    route_wake_pollers_locked();
}

/* ── Public entry points ──────────────────────────────────────── */

void input_route_set_normal_sinks(const input_route_kbd_sink_t *kbd,
                                  const input_route_mouse_sink_t *mouse)
{
    uint64_t flags = spin_lock_irqsave(&route.lock);

    if (kbd)
        route.kbd_sink = kbd;
    if (mouse)
        route.mouse_sink = mouse;

    spin_unlock_irqrestore(&route.lock, flags);
}

void input_route_init(void)
{
    /* The sinks are configuration, not state: they survive the reset
     * so that input_route_init() stays idempotent. */
    const input_route_kbd_sink_t   *kbd_sink   = route.kbd_sink;
    const input_route_mouse_sink_t *mouse_sink = route.mouse_sink;

    /* Zero EVERYTHING first: input_ps2_frame_reset() does not clear
     * the framer's per-sequence token, and a stale token would make
     * the next multi-byte sequence fail the accumulator check. */
    memset(&route, 0, sizeof(route));
    route.kbd_sink = kbd_sink;
    route.mouse_sink = mouse_sink;

    spin_init(&route.lock);
    list_init(&route.poll);
    input_ps2_frame_reset(&route.framer);
    route.epoch = 1;
}

int input_route_open(input_device_t device, struct file **out)
{
    struct route_endpoint *ep;
    file_t *f;

    if (!out)
        return -EINVAL;
    if (device != INPUT_DEVICE_KEYBOARD && device != INPUT_DEVICE_MOUSE)
        return -EINVAL;

    ep = (struct route_endpoint *)kmalloc(sizeof(*ep));
    if (!ep)
        return -ENOMEM;
    memset(ep, 0, sizeof(*ep));
    ep->device = device;

    f = file_alloc();
    if (!f) {
        kfree(ep);
        return -ENOMEM;
    }
    f->type = FD_DEV;
    f->dev_private = ep;

    *out = f;
    return 0;
}

void input_route_release_file(struct file *f)
{
    struct route_endpoint *ep;
    uint64_t flags;

    if (!f)
        return;
    ep = (struct route_endpoint *)f->dev_private;
    if (!ep)
        return;   /* not a route file */

    flags = spin_lock_irqsave(&route.lock);

    /* Closing ANY bound file revokes the whole lease. */
    if (route.lease_active && (ep == route.kbd_ep || ep == route.mouse_ep))
        route_revoke_locked(0);

    /* Unlink the endpoint: the lease only holds weak identities, so
     * nothing keeps the final close from ever firing. */
    if (route.kbd_ep == ep)
        route.kbd_ep = NULL;
    if (route.mouse_ep == ep)
        route.mouse_ep = NULL;

    ep->revoked = 1;
    if (!ep->error)
        ep->error = EIO;

    spin_unlock_irqrestore(&route.lock, flags);

    /* The endpoint is freed only after the lock is dropped. */
    f->dev_private = NULL;
    kfree(ep);
}

int input_route_bind(const void *view, struct file *kbd, struct file *mouse,
                     uint64_t *generation, uint32_t *initial_modifiers)
{
    struct route_endpoint *kbd_ep, *mouse_ep;
    uint32_t mods;
    uint64_t flags;
    int i;

    if (!view || !kbd)
        return -EINVAL;

    kbd_ep = (struct route_endpoint *)kbd->dev_private;
    if (!kbd_ep || kbd_ep->device != INPUT_DEVICE_KEYBOARD || kbd_ep->revoked)
        return -EINVAL;

    /* A keyboard-only lease (mouse == NULL) leases the keyboard alone:
     * the mouse device stays on the NORMAL path. */
    if (mouse) {
        mouse_ep = (struct route_endpoint *)mouse->dev_private;
        if (!mouse_ep || mouse_ep->device != INPUT_DEVICE_MOUSE || mouse_ep->revoked)
            return -EINVAL;
    } else {
        mouse_ep = NULL;
    }

    flags = spin_lock_irqsave(&route.lock);

    /* A new bind replaces the live lease: the previous owner loses it. */
    if (route.lease_active)
        route_revoke_locked(ESTALE);

    route.lease_active = 1;
    route.view = view;
    route.generation = ++route.next_generation;   /* non-zero, monotonic */
    route.epoch++;                                /* a new epoch */
    route.kbd_ep = kbd_ep;
    route.mouse_ep = mouse_ep;

    kbd_ep->generation = route.generation;
    kbd_ep->error = 0;
    kbd_ep->revoked = 0;
    if (mouse_ep) {
        mouse_ep->generation = route.generation;
        mouse_ep->error = 0;
        mouse_ep->revoked = 0;
    }

    /* The snapshot the new consumer starts from: the modifiers that
     * are physically held plus the Caps latch. */
    mods = route_physical_modifiers_locked();

    /* Every key that is physically held but NOT reported by the
     * snapshot is isolated: its release must never reach the new
     * consumer as a phantom release.
     *
     * The held MODIFIER keys ARE reported by the snapshot, so their
     * release must reach the consumer too: any isolation an earlier
     * epoch boundary installed on them is cleared here.  Without that,
     * the snapshot could report "Ctrl is held" while the matching
     * release was swallowed, and the consumer would wait for a release
     * that never comes. */
    for (i = 0; i < ROUTE_KEY_SLOTS; i++) {
        if (!route.phys_down[i])
            continue;
        if (route_modifier_bit((uint16_t)i)) {
            route.isolated[i] = 0;
            continue;
        }
        route.isolated[i] = 1;
    }

    /* A new lease starts from a known-consistent state: the DESYNC flag
     * belongs to the lease that is being replaced. */
    route.kbd_desync = 0;

    /* The mouse buttons that were already held are isolated as well:
     * the consumer never saw their press, so it must never see them
     * held.  The movement is never lost.  A keyboard-only lease does
     * not own the mouse, so its buttons are left alone. */
    route.mouse_isolated = mouse_ep ? route.mouse_buttons : 0;

    /* The transient modifier state starts clean. */
    route.logical_mods = 0;

    if (generation)
        *generation = route.generation;
    if (initial_modifiers)
        *initial_modifiers = mods;

    spin_unlock_irqrestore(&route.lock, flags);
    return 0;
}

int input_route_lease_live(const void **view, uint64_t *generation)
{
    uint64_t flags;
    int live;

    flags = spin_lock_irqsave(&route.lock);

    live = route.lease_active ? 1 : 0;
    if (live) {
        if (view) *view = route.view;
        if (generation) *generation = route.generation;
    }

    spin_unlock_irqrestore(&route.lock, flags);
    return live;
}

void input_route_unbind(const void *view, uint64_t generation, int reason)
{
    uint64_t flags;

    if (!view)
        return;

    flags = spin_lock_irqsave(&route.lock);

    if (!route.lease_active) {
        spin_unlock_irqrestore(&route.lock, flags);
        return;
    }
    /* Another view's cleanup never touches this lease. */
    if (route.view != view) {
        spin_unlock_irqrestore(&route.lock, flags);
        return;
    }
    /* A non-zero generation must match exactly.  Zero is the current
     * view's explicit release. */
    if (generation != 0 && generation != route.generation) {
        spin_unlock_irqrestore(&route.lock, flags);
        return;
    }

    route_revoke_locked(reason);

    spin_unlock_irqrestore(&route.lock, flags);
}

int input_route_read(struct file *f, uint64_t offset, uint64_t size, void *buf)
{
    struct route_endpoint *ep;
    uint64_t flags;
    int rc;

    if (!f || !buf)
        return -EINVAL;
    ep = (struct route_endpoint *)f->dev_private;
    if (!ep)
        return -EINVAL;

    flags = spin_lock_irqsave(&route.lock);

    if (ep->revoked && ep->error) {
        rc = -ep->error;
    } else if (route_device_is_leased_locked(ep) &&
               !route_endpoint_is_owner_locked(ep)) {
        /* The lease is exclusive: no other reader may steal the bytes. */
        rc = -EBUSY;
    } else if (route_endpoint_is_owner_locked(ep)) {
        rc = route_active_read_locked(ep, size, buf);
    } else {
        rc = route_normal_read_locked(ep, offset, size, buf);
    }

    spin_unlock_irqrestore(&route.lock, flags);
    return rc;
}

uint32_t input_route_poll(struct file *f, uint32_t requested,
                           struct poll_table *pt)
{
    struct route_endpoint *ep;
    uint64_t flags;
    uint32_t mask = 0;
    int pending, overflow, desync;

    if (!f)
        return POLLNVAL;
    ep = (struct route_endpoint *)f->dev_private;
    if (!ep)
        return POLLNVAL;

    flags = spin_lock_irqsave(&route.lock);

    /* The readiness of the ACTIVE queues is per device: the keyboard
     * queue must never make the mouse file look ready (and the other
     * way round). */
    if (ep->device == INPUT_DEVICE_KEYBOARD) {
        pending = route.kbd_count;
        overflow = route.kbd_overflow;
        desync = route.kbd_desync;
    } else {
        pending = route.mouse_count;
        overflow = route.mouse_overflow;
        desync = 0;
    }

    if (ep->revoked && ep->error) {
        mask = POLLERR;
    } else if (route_device_is_leased_locked(ep) &&
               !route_endpoint_is_owner_locked(ep)) {
        mask = POLLERR;
    } else if (route_endpoint_is_owner_locked(ep)) {
        if (overflow || desync) {
            /* The overflow means the stream lost data, the desync means
             * the consumer's key state may have diverged: both are
             * reported the same way, and the read tells the consumer
             * which one it is (-EOVERFLOW / -ESTALE). */
            mask = POLLERR;
        } else if (pending) {
            mask = POLLIN | POLLRDNORM;
        } else if (poll_requested_read(requested) && pt && !pt->triggered) {
            poll_wait(pt, &route.poll, &route.lock);
            /* Re-check under the SAME critical section: a publish that
             * raced the registration would have found no waiter, and
             * the wakeup would be lost. */
            if (ep->device == INPUT_DEVICE_KEYBOARD) {
                pending = route.kbd_count;
                overflow = route.kbd_overflow;
                desync = route.kbd_desync;
            } else {
                pending = route.mouse_count;
                overflow = route.mouse_overflow;
                desync = 0;
            }
            if (overflow || desync) {
                mask = POLLERR;
            } else if (pending) {
                mask = POLLIN | POLLRDNORM;
                route_wake_pollers_locked();
            }
        }
    } else {
        mask = route_normal_poll_locked(ep, requested, pt);
    }

    spin_unlock_irqrestore(&route.lock, flags);
    return mask;
}

/* ── The producer ─────────────────────────────────────────────── */

/* Publish one COMPLETE sequence.  The route lock is held. */
static void route_publish_frame_locked(const input_ps2_frame_t *frame)
{
    uint16_t key = frame->physical_key;
    int ordinary = (!frame->is_special && key < ROUTE_KEY_SLOTS);

    /* 1. The physical state always advances, even for a sequence that
     *    is swallowed below: the key really is held. */
    if (ordinary) {
        if (frame->is_make) {
            if (!route.phys_down[key]) {
                route.phys_down[key] = 1;
                /* The Caps latch flips on the UP → DOWN transition only,
                 * so the auto-repeat of a held Caps key never flips it
                 * again. */
                if (key == KBD_CAPS)
                    route.caps_on = !route.caps_on;
            }
        } else {
            route.phys_down[key] = 0;
        }
    }

    /* 2. The transient modifier state follows the events. */
    if (ordinary) {
        uint32_t bit = route_modifier_bit(key);
        if (bit) {
            if (frame->is_make)
                route.logical_mods |= bit;
            else
                route.logical_mods &= ~bit;
        }
    }

    /* 3. A sequence that crosses an epoch boundary started in another
     *    lease's era: it is isolated, so the consumer never sees a
     *    half-truth.  The key stays isolated until its break.
     *
     *    The consumer's key state may now have diverged from the
     *    physical one (it never saw this sequence), so the ACTIVE path
     *    goes DESYNC and says so OUT OF BAND: the byte stream stays raw
     *    Set 1 payload, the flag is reported by the read (-ESTALE) and
     *    by the poll (POLLERR). */
    if (frame->start_epoch != route.epoch) {
        if (route.lease_active)
            route.kbd_desync = 1;
        if (ordinary) {
            if (frame->is_make)
                route.isolated[key] = 1;
            else
                route.isolated[key] = 0;
        }
        return;
    }

    /* 4. An isolated key is swallowed until its break. */
    if (ordinary && route.isolated[key]) {
        if (!frame->is_make)
            route.isolated[key] = 0;
        return;
    }

    /* 5. Publish the whole sequence. */
    if (route.lease_active) {
        route_kbd_push_locked(frame->bytes, frame->length);
    } else if (route.kbd_sink && route.kbd_sink->publish) {
        route.kbd_sink->publish(frame, route_logical_snapshot_locked());
    }
}

void input_route_keyboard_byte(uint8_t byte)
{
    uint64_t flags;
    int rc;

    flags = spin_lock_irqsave(&route.lock);

    /* The accumulator is the route's own frame: the same object with a
     * stable address for every byte of one sequence. */
    rc = input_ps2_frame_feed(&route.framer, byte, route.epoch, &route.frame);
    if (rc == 1)
        route_publish_frame_locked(&route.frame);
    /* rc == 0: the sequence is still incomplete — nothing is published.
     * rc < 0:  -EINVAL means the sequence is LOST (no state update, no
     * publication, no resumption) and -EILSEQ means the byte stream is
     * corrupt (the offending byte is consumed, nothing is published). */

    spin_unlock_irqrestore(&route.lock, flags);
}

void input_route_mouse_event(const mouse_event_t *event)
{
    uint64_t flags;
    mouse_event_t out;

    if (!event)
        return;

    flags = spin_lock_irqsave(&route.lock);

    out = *event;
    if (route.mouse_isolated) {
        /* The isolated buttons are masked out of the published state:
         * the consumer never saw their press, so it must never see
         * them held.  The MOVEMENT is never lost. */
        out.buttons = (uint8_t)(event->buttons & (uint8_t)~route.mouse_isolated);
        /* A button that was isolated and is now released leaves the
         * isolation set. */
        route.mouse_isolated = (uint8_t)(route.mouse_isolated & event->buttons);
    }
    route.mouse_buttons = event->buttons;

    if (route.lease_active && route.mouse_ep) {
        /* The mouse is leased: the event belongs to the lease. */
        route_mouse_push_locked(&out);
    } else if (route.mouse_sink && route.mouse_sink->publish) {
        /* A keyboard-only lease does not own the mouse: the mouse stays
         * on the NORMAL path. */
        route.mouse_sink->publish(&out);
    }

    spin_unlock_irqrestore(&route.lock, flags);
}
