/*
 * test_input_route.c — the exclusive physical-input lease
 *
 * GUI-2 plan Task 3.  The route owns the physical key state, decides
 * which reader may consume the PS/2 bytes, and publishes whole
 * sequences either to the legacy NORMAL path or to the leased ACTIVE
 * path.  The lease is exclusive: while it is live, the owner's file
 * reads the ACTIVE queue and every other file on the device is shut
 * out with EBUSY / POLLERR.
 *
 * The fixture host-compiles the REAL kernel/driver/input_route.c and
 * kernel/driver/input_ps2.c and links them against the recording
 * runtime in hosttests/mock/input_route_test_runtime.{c,h}.  The
 * recording runtime exists because the route's contract is a lock
 * ORDER and an atomic "authorize + dequeue + register waiter" window:
 * a no-op mock spinlock cannot show either, so the runtime records
 * every acquisition with its nesting depth and the assertions look at
 * the recorded order.
 */

#include <test_framework.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <log/log.h>
#include <driver/input_route.h>
#include <driver/input_ps2.h>
#include <uapi/mouse.h>

/* ── Fixture helpers ─────────────────────────────────────────── */

static int view_token;                 /* the weak view identity */
static const void *view(void) { return &view_token; }

/* The opened route files, so every test starts from a clean pool and
 * the address-reuse case is deterministic. */
#define TRACKED_MAX 32
static file_t *tracked[TRACKED_MAX];
static int     tracked_count;

static file_t *route_open(input_device_t device)
{
    file_t *f = NULL;
    if (input_route_open(device, &f) != 0)
        return NULL;
    if (tracked_count < TRACKED_MAX)
        tracked[tracked_count++] = f;
    return f;
}

static void route_forget(file_t *f)
{
    int i;
    for (i = 0; i < tracked_count; i++) {
        if (tracked[i] == f) {
            tracked[i] = tracked[--tracked_count];
            return;
        }
    }
}

/* Drop one reference; the file is forgotten once it is really gone. */
static void route_close(file_t *f)
{
    if (!f)
        return;
    file_put(f);
    if (f->refcount == 0)
        route_forget(f);
}

static void route_reset(void)
{
    int i;
    for (i = 0; i < tracked_count; i++) {
        while (tracked[i]->refcount > 0)
            file_put(tracked[i]);
    }
    tracked_count = 0;

    input_route_init();
    input_route_set_normal_sinks(&input_route_test_kbd_sink,
                                 &input_route_test_mouse_sink);
    input_route_test_normal_reset();
    input_route_test_trace_reset();
    input_route_test_set_release_hook(input_route_release_file);
    input_route_test_set_poll_wait_hook(NULL);
    input_route_test_wake_reset();
    input_route_test_warn_reset();
}

static void feed(const uint8_t *bytes, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) input_route_keyboard_byte(bytes[i]);
}

static void feed1(uint8_t byte)
{
    input_route_keyboard_byte(byte);
}

/* The publish injected at the check-empty → register-waiter boundary. */
static void feed_hook_publish(void)
{
    input_route_keyboard_byte(0x1E);
}

/* The read result is returned VERBATIM: a positive value is the byte
 * count, 0 is "no bytes" and a NEGATIVE value is the errno the route
 * refused with (-EAGAIN "nothing yet", -EBUSY "not the owner",
 * -EOVERFLOW "the stream was lost").  Collapsing the errno to 0 would
 * make every "the route refused" assertion vacuous. */
static int64_t drain_bytes(file_t *f, uint8_t *buf, size_t size)
{
    int64_t rc;
    memset(buf, 0, size);
    rc = input_route_read(f, 0, size, buf);
    return rc;
}

static size_t drain_events(file_t *f, mouse_event_t *buf, size_t max)
{
    int rc;
    memset(buf, 0, max * sizeof(mouse_event_t));
    rc = input_route_read(f, 0, max * sizeof(mouse_event_t), buf);
    return rc > 0 ? (size_t)rc / sizeof(mouse_event_t) : 0;
}

/* ── Before the lease the device is NORMAL ───────────────────── */

static void test_normal_path_before_bind(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[8];

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_not_null(kbd);
    assert_not_null(mouse);

    /* No lease: the bytes go to the NORMAL queue. */
    feed1(0x1E);
    assert_eq(1, input_route_test_normal_kbd_bytes());

    assert_eq(1, drain_bytes(kbd, buf, sizeof(buf)));
    assert_eq(0x1E, buf[0]);
    assert_eq(0, input_route_test_normal_kbd_bytes());

    /* And the NORMAL queue drives the poll. */
    feed1(0x1E);
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLIN) != 0);
}

/* ── The lease: the owner reads, everybody else is shut out ──── */

static void test_lease_owner_reads_others_busy(void)
{
    file_t *kbd, *mouse, *other;
    uint8_t buf[8];
    uint64_t generation = 0;
    uint32_t modifiers = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    other = route_open(INPUT_DEVICE_KEYBOARD);

    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, &modifiers));
    assert_true(generation != 0);

    feed1(0x1E);

    /* The owner reads the ACTIVE queue. */
    assert_eq(1, drain_bytes(kbd, buf, sizeof(buf)));
    assert_eq(0x1E, buf[0]);
    /* The NORMAL queue stays empty: the bytes did not leak. */
    assert_eq(0, input_route_test_normal_kbd_bytes());

    /* Every other file on the device is shut out. */
    assert_eq(-EBUSY, input_route_read(other, 0, sizeof(buf), buf));
    assert_true((input_route_poll(other, POLLIN, NULL) & POLLERR) != 0);
    /* The owner is ready, not failed. */
    feed1(0x1E);
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLIN) != 0);
    assert_false((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
}

/* ── An empty ACTIVE queue is EAGAIN ─────────────────────────── */

static void test_empty_active_queue_is_eagain(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[8];

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    assert_eq(-EAGAIN, input_route_read(kbd, 0, sizeof(buf), buf));
}

/* ── Only the FINAL close revokes ───────────────────────────── */

static void test_dup_revokes_only_on_final_close(void)
{
    file_t *kbd, *mouse, *other;
    uint8_t buf[8];
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    other = route_open(INPUT_DEVICE_KEYBOARD);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));

    /* A second reference to the same open file description. */
    file_get(kbd);
    assert_eq(2, kbd->refcount);

    /* Dropping one reference is not the final close: the lease lives. */
    route_close(kbd);
    assert_eq(-EBUSY, input_route_read(other, 0, sizeof(buf), buf));

    /* The final close revokes the lease. */
    route_close(kbd);
    assert_eq(0, input_route_read(other, 0, sizeof(buf), buf));
}

/* ── Closing ANY bound file revokes the whole lease ─────────── */

static void test_close_any_bound_file_revokes_lease(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[8];

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    feed1(0x1E);
    assert_eq(1, drain_bytes(kbd, buf, sizeof(buf)));

    /* The mouse file is part of the same lease: closing it tears the
     * whole lease down, so the keyboard file falls back to NORMAL. */
    route_close(mouse);
    assert_eq(0, input_route_read(kbd, 0, sizeof(buf), buf));
    feed1(0x1E);
    assert_eq(0, input_route_test_normal_kbd_bytes());
    feed1(0x9E);
    feed1(0x1E);
    assert_eq(1, input_route_test_normal_kbd_bytes());
}

/* ── A recycled file address must not release a new lease ────── */

static void test_recycled_address_does_not_release_new_lease(void)
{
    file_t *first, *second, *mouse;
    uint64_t gen1 = 0, gen2 = 0, gen3 = 0;

    route_reset();
    first = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), first, mouse, &gen1, NULL));

    /* The final close of the first file revokes the lease. */
    route_close(first);

    /* The pool hands the very same address out again. */
    second = route_open(INPUT_DEVICE_KEYBOARD);
    assert_true(second == first);

    /* A new lease on the recycled address. */
    assert_eq(0, input_route_bind(view(), second, mouse, &gen2, NULL));
    assert_true(gen2 > gen1);

    /* Revoke it through the generation, then close the recycled file:
     * the stale record must not release anything twice. */
    input_route_unbind(view(), gen2, 0);
    route_close(second);
    assert_eq(0, input_route_test_alloc_double_free());
    assert_eq(0, input_route_test_file_double_free());

    /* The next lease is still handed out cleanly. */
    second = route_open(INPUT_DEVICE_KEYBOARD);
    assert_eq(0, input_route_bind(view(), second, mouse, &gen3, NULL));
    assert_true(gen3 > gen2);
}

/* ── Overflow: a sequence that does not fit, the 65th event ──── */

static void test_keyboard_active_queue_overflow(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[8];
    int i;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    /* Fill the queue to (almost) its usable capacity of 255 bytes. */
    for (i = 0; i < 254; i++) feed1(0x1E);

    /* A two-byte sequence does not fit any more: it is rejected as a
     * whole, never truncated. */
    feed1(0xE0);
    feed1(0x1D);

    assert_eq(-EOVERFLOW, input_route_read(kbd, 0, sizeof(buf), buf));
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
    /* The mouse queue is a different queue: the keyboard overflow does
     * not make the mouse file look failed. */
    assert_false((input_route_poll(mouse, POLLIN, NULL) & POLLERR) != 0);
}

static void test_mouse_active_queue_overflow(void)
{
    file_t *kbd, *mouse;
    mouse_event_t events[80];
    mouse_event_t ev;
    int i;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    memset(&ev, 0, sizeof(ev));
    ev.dx = 1;
    for (i = 0; i < 65; i++) input_route_mouse_event(&ev);

    assert_eq(-EOVERFLOW, input_route_read(mouse, 0, sizeof(events), events));
    assert_true((input_route_poll(mouse, POLLIN, NULL) & POLLERR) != 0);
}

/* ── A sequence is never published in halves ─────────────────── */

static void test_boundary_sequences_never_publish_half(void)
{
    static const uint8_t e0[]        = { 0xE0, 0x1D };
    static const uint8_t printscr[]   = { 0xE0, 0x2A, 0xE0, 0x37 };
    static const uint8_t pause[]      = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    static const struct {
        const uint8_t *seq;
        size_t len;
    } cases[] = {
        { e0, sizeof(e0) },
        { printscr, sizeof(printscr) },
        { pause, sizeof(pause) },
    };
    /* Every revoke reason, including the explicit cleanup (0). */
    static const int reasons[] = { 0, EACCES, ESTALE, EIO };
    size_t c;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t split;
        size_t r;

        /* The whole sequence at once is published whole. */
        route_reset();
        {
            file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
            file_t *mouse = route_open(INPUT_DEVICE_MOUSE);
            uint8_t buf[16];
            assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));
            feed(cases[c].seq, cases[c].len);
            assert_eq(cases[c].len, drain_bytes(kbd, buf, sizeof(buf)));
        }

        /* At every fragment point and with EVERY revoke reason: no half
         * sequence, on the ACTIVE path and on the NORMAL path. */
        for (r = 0; r < sizeof(reasons) / sizeof(reasons[0]); r++) {
            for (split = 1; split < cases[c].len; split++) {
                file_t *kbd, *mouse;
                uint8_t buf[16];
                int64_t got;
                size_t  normal;

                route_reset();
                kbd = route_open(INPUT_DEVICE_KEYBOARD);
                mouse = route_open(INPUT_DEVICE_MOUSE);
                assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

                feed(cases[c].seq, split);
                /* Nothing is published yet: the ACTIVE queue is empty, so
                 * the read says EAGAIN ("no bytes") — not "0 bytes", which
                 * a collapsed errno cannot express. */
                assert_eq(-EAGAIN, drain_bytes(kbd, buf, sizeof(buf)));

                /* Revoke in the middle of the sequence. */
                input_route_unbind(view(), 0, reasons[r]);

                if (reasons[r] == 0) {
                    /* An explicit cleanup: the endpoint falls back to the
                     * NORMAL path and can be leased again. */
                    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));
                } else {
                    /* A revoked endpoint is dead: it reports the reason
                     * and it cannot be leased again. */
                    assert_eq(-reasons[r],
                              input_route_read(kbd, 0, sizeof(buf), buf));
                    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
                    assert_true(input_route_bind(view(), kbd, mouse, NULL, NULL) < 0);
                }

                feed(cases[c].seq + split, cases[c].len - split);

                /* The ACTIVE path: the fragment crossed the epoch boundary,
                 * so it was ISOLATED — it is never published, whole or
                 * half.  A live endpoint says so out of band (-EOVERFLOW /
                 * DESYNC); a dead one keeps reporting its revoke reason.
                 * The two are DIFFERENT answers, so the assertion has to
                 * tell them apart instead of accepting any zero. */
                got = drain_bytes(kbd, buf, sizeof(buf));
                if (reasons[r] == 0) {
                    assert_eq(-EOVERFLOW, got);
                } else {
                    assert_eq(-reasons[r], got);
                }
                /* The NORMAL path is held to the same rule: an isolated
                 * fragment never reaches it either. */
                normal = input_route_test_normal_kbd_bytes();
                assert_eq(0, normal);
            }
        }
    }
}

/* ── A make that crosses an epoch is isolated until its break ── */

static void test_cross_epoch_make_is_isolated_until_break(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[16];
    uint64_t generation = 0;
    uint32_t modifiers = 0;
    int64_t got;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);

    /* The sequence starts in the pre-lease epoch. */
    feed1(0xE0);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, &modifiers));

    /* The make completes in the new epoch: it is isolated, so the
     * consumer never sees a half-truth — and the ACTIVE stream says so
     * out of band (-EOVERFLOW / DESYNC). */
    feed1(0x1D);
    assert_eq(-EOVERFLOW, drain_bytes(kbd, buf, sizeof(buf)));

    /* The physical state still advanced: the key IS held. */
    input_route_unbind(view(), generation, 0);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, &modifiers));
    assert_true((modifiers & INPUT_ROUTE_MOD_CTRL) != 0);
    /* The new lease is consistent again: the queue is empty, so the read
     * says EAGAIN. */
    assert_eq(-EAGAIN, drain_bytes(kbd, buf, sizeof(buf)));

    /* The physical snapshot remains internal; the UAPI exposes Caps
     * alone.  The new consumer saw no Ctrl press, so isolate its break. */
    feed1(0xE0);
    feed1(0x9D);
    got = drain_bytes(kbd, buf, sizeof(buf));
    assert_eq(-EAGAIN, got);

    /* The next make is published normally. */
    feed1(0xE0);
    feed1(0x1D);
    assert_eq(2, drain_bytes(kbd, buf, sizeof(buf)));
    assert_eq(0xE0, buf[0]);
    assert_eq(0x1D, buf[1]);
}

/* ── The cross-epoch make puts the ACTIVE path into DESYNC ──── */

static void test_cross_epoch_make_reports_desync(void)
{
    file_t *kbd, *mouse, *other;
    uint8_t buf[16];
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    other = route_open(INPUT_DEVICE_KEYBOARD);

    /* The sequence starts in the pre-lease epoch, completes in the
     * lease's epoch. */
    feed1(0xE0);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));
    feed1(0x1D);

    /* The frame was swallowed, but the consumer is TOLD that its key
     * state may have diverged: the poll reports POLLERR ... */
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
    /* ... and it stays reported: polling does not consume the flag. */
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
    /* ... and the read tells the consumer which error it is.  The spec
     * §4.2 state table names EOVERFLOW for the DESYNC state. */
    assert_eq(-EOVERFLOW, input_route_read(kbd, 0, sizeof(buf), buf));

    /* STICKY: the prohibition continues until the app cleans up or the
     * lease is finally closed.  The read is NOT consumed by the first
     * read: the second one fails the same way. */
    assert_eq(-EOVERFLOW, input_route_read(kbd, 0, sizeof(buf), buf));
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);

    /* A file that is not the lease owner is shut out with EBUSY: it
     * never sees the owner's error, and the owner's error never
     * downgrades the owner's own lease to "busy". */
    assert_eq(-EBUSY, input_route_read(other, 0, sizeof(buf), buf));
    assert_true((input_route_poll(other, POLLIN, NULL) & POLLERR) != 0);
}

/* ── A new lease starts from a known-consistent state ────────── */

static void test_desync_is_cleared_at_bind(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[16];
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);

    feed1(0xE0);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));
    feed1(0x1D);
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);

    /* The sticky prohibition lasts for the WHOLE lease: the read keeps
     * reporting the error, no matter how often it is retried. */
    assert_eq(-EOVERFLOW, input_route_read(kbd, 0, sizeof(buf), buf));
    assert_eq(-EOVERFLOW, input_route_read(kbd, 0, sizeof(buf), buf));

    /* The app cleans up: the lease is released. */
    input_route_unbind(view(), generation, 0);

    /* The DESYNC flag SURVIVES the release: it belongs to the lease, and
     * a release is not a bind.  This is what makes the clearing below the
     * BIND's work — a clear at the release would fail HERE, even though
     * the release happened before the new lease. */
    assert_eq(1, input_route__test_desync_active());

    /* A new lease starts from a known-consistent state: the previous
     * lease's DESYNC does not follow it.
     *
     * THE CLEARING HAPPENS AT BIND — not at the release above — so this
     * assertion fails if the bind-time reset is removed, even though the
     * release already happened. */
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));
    assert_eq(0, input_route__test_desync_active());
    assert_false((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
    assert_eq(-EAGAIN, input_route_read(kbd, 0, sizeof(buf), buf));
}

/* ── Binding while a key is held produces no new DOWN ───────── */

static void test_bind_with_held_key_produces_no_new_down(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[16];
    uint32_t modifiers = 0;
    int64_t got;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);

    /* Shift goes down before the lease: it belongs to the NORMAL path. */
    feed1(0x2A);
    assert_eq(1, input_route_test_normal_kbd_bytes());

    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, &modifiers));
    /* The snapshot tells the new consumer what is already held. */
    assert_true((modifiers & INPUT_ROUTE_MOD_SHIFT) != 0);

    /* The consumer saw no press: the held key release stays isolated. */
    feed1(0xAA);
    got = drain_bytes(kbd, buf, sizeof(buf));
    assert_eq(-EAGAIN, got);
}

/* ── The Caps typematic must not flip the latch ─────────────── */

static void test_caps_typematic_does_not_flip(void)
{
    file_t *kbd, *mouse;
    uint32_t modifiers = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);

    feed1(0x3A);                 /* the Caps make: ON */
    feed1(0x3A);                 /* the auto-repeat: no flip */
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, &modifiers));
    assert_true((modifiers & INPUT_ROUTE_MOD_CAPS) != 0);

    input_route_unbind(view(), 0, 0);
    feed1(0xBA);                 /* the Caps release: the latch stays */
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, &modifiers));
    assert_true((modifiers & INPUT_ROUTE_MOD_CAPS) != 0);

    input_route_unbind(view(), 0, 0);
    feed1(0x3A);                 /* the next make: OFF */
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, &modifiers));
    assert_false((modifiers & INPUT_ROUTE_MOD_CAPS) != 0);
}

/* ── The mouse button isolation keeps the movement ───────────── */

static void test_mouse_button_isolation_keeps_movement(void)
{
    file_t *kbd, *mouse;
    mouse_event_t ev, events[4];

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);

    /* The button goes down before the lease. */
    memset(&ev, 0, sizeof(ev));
    ev.buttons = 0x01;
    ev.dx = 0;
    input_route_mouse_event(&ev);

    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    /* The movement after the lease is delivered; the button the
     * consumer never saw pressed is masked out of the state. */
    memset(&ev, 0, sizeof(ev));
    ev.buttons = 0x01;
    ev.dx = 7;
    input_route_mouse_event(&ev);

    assert_eq(1, drain_events(mouse, events, 4));
    assert_eq(7, events[0].dx);
    assert_eq(0, events[0].buttons);

    /* The release of the isolated button is swallowed as well. */
    memset(&ev, 0, sizeof(ev));
    ev.buttons = 0x00;
    ev.dx = 3;
    input_route_mouse_event(&ev);
    assert_eq(1, drain_events(mouse, events, 4));
    assert_eq(0, events[0].buttons);
}

/* ── The lock order: route → NORMAL queue ────────────────────── */

static void test_lock_order_is_route_then_normal_queue(void)
{
    const void *route_lock;

    route_reset();
    feed1(0x1E);

    /* The route lock is the first lock the route takes. */
    route_lock = input_route_test_trace_first_lock();
    assert_not_null(route_lock);

    /* The NORMAL queue's own lock is taken INSIDE the route lock. */
    assert_eq(2, input_route_test_trace_first_depth(input_route_test_device_lock()));
    /* The recorded trace is balanced. */
    assert_eq(0, input_route_test_trace_mismatch());
    assert_eq(0, input_route_test_trace_depth_now());
}

/* ── No lost wakeup at the check → register boundary ─────────── */

static void test_no_lost_wakeup_at_register_boundary(void)
{
    file_t *kbd, *mouse;
    poll_table_t pt;
    const void *route_lock;
    uint32_t mask;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    assert_eq(0, poll_table_setup(&pt, 4));

    /* The publish lands between the readiness check and the waiter
     * registration — the classic lost-wakeup window. */
    input_route_test_set_poll_wait_hook(feed_hook_publish);
    mask = input_route_poll(kbd, POLLIN, &pt);
    input_route_test_set_poll_wait_hook(NULL);

    /* The publish was not lost: the poll reports the data. */
    assert_true((mask & POLLIN) != 0);
    /* And the registered waiter was woken. */
    assert_true(input_route_test_wake_count() > 0);

    /* The mock lock is not a no-op: the re-entrant acquisition the
     * injection forces is visible in the recorded trace. */
    route_lock = input_route_test_trace_first_lock();
    assert_not_null(route_lock);
    assert_true(input_route_test_trace_reentry_depth(route_lock) > 0);
    assert_eq(0, input_route_test_trace_mismatch());

    poll_table_destroy(&pt);
}

/* ── The unbind generation rules ─────────────────────────────── */

static void test_unbind_generation_rules(void)
{
    file_t *kbd, *mouse, *other;
    uint8_t buf[8];
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    other = route_open(INPUT_DEVICE_KEYBOARD);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));
    assert_true(generation != 0);

    /* A wrong generation must not release the lease. */
    input_route_unbind(view(), generation + 1, 0);
    assert_eq(-EBUSY, input_route_read(other, 0, sizeof(buf), buf));

    /* Another view must not release the lease either. */
    input_route_unbind(&view_token + 1, generation, 0);
    assert_eq(-EBUSY, input_route_read(other, 0, sizeof(buf), buf));

    /* The exact generation releases it. */
    input_route_unbind(view(), generation, 0);
    assert_eq(0, input_route_read(other, 0, sizeof(buf), buf));

    /* A repeated cleanup is a no-op. */
    input_route_unbind(view(), generation, 0);
    assert_eq(0, input_route_read(other, 0, sizeof(buf), buf));
}

/* ── A revoked lease reports the reason ──────────────────────── */

static void test_revoked_lease_reports_the_reason(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[8];
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));

    input_route_unbind(view(), generation, EACCES);

    assert_eq(-EACCES, input_route_read(kbd, 0, sizeof(buf), buf));
    assert_true((input_route_poll(kbd, POLLIN, NULL) & POLLERR) != 0);
}

/* ── The revoke resets the transient modifier state ───────────── */

/*
 * The shell must not inherit the modifier keys the GUI session held:
 * the route resets its transient modifier state when the lease is
 * revoked, while the PHYSICAL state (the keys are still held) is not.
 *
 * The NORMAL sink is the observer: after the lease is gone the next
 * frame is published there with the route's logical modifier snapshot,
 * and that snapshot must not carry the modifier the GUI session held.
 */
static void test_revoke_resets_the_transient_modifier_state(void)
{
    file_t *kbd, *mouse;
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);

    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));

    /* Ctrl goes down inside the lease: the transient modifier state
     * follows the events, so the route now holds Ctrl. */
    feed1(0x1D);
    assert_eq(0, input_route_test_normal_kbd_bytes());

    /* The lease is released. */
    input_route_unbind(view(), generation, 0);

    /* The next frame goes to the NORMAL path — the shell's — and the
     * modifier snapshot it carries must NOT contain the modifier the
     * GUI session held. */
    feed1(0x1E);
    assert_eq(1, input_route_test_normal_kbd_bytes());
    assert_eq(0, input_route_test_normal_kbd_modifiers());
}

/* ── The overflow warning is rate-limited, never silenced ─────── */

/*
 * Under a sustained overflow the warning fired on EVERY rejected
 * sequence and flooded the serial console — the one channel the
 * recovery canary reads.  The FIRST occurrence of each distinct
 * condition must still be logged (the user has to learn that the
 * input stream was lost); the repeats are suppressed.
 */
static void test_overflow_warning_is_rate_limited_but_not_silent(void)
{
    file_t *kbd, *mouse;
    uint64_t generation = 0;
    uint8_t buf[8];
    mouse_event_t events[80];
    mouse_event_t ev;
    int i;
    int saved_level = g_log_level;

    /* The warning is gated on the log level: raise it so the mock's
     * counter sees the message. */
    g_log_level = LOG_WARN;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));

    /* Fill the ACTIVE keyboard queue, then keep pushing sequences that
     * do not fit: every one of them is rejected. */
    for (i = 0; i < 254; i++) feed1(0x1E);
    for (i = 0; i < 20; i++) {
        feed1(0xE0);
        feed1(0x1D);
    }

    /* The first keyboard overflow was logged ... */
    assert_eq(1, input_route_test_warn_kbd_overflow());
    /* ... and the repeats were not: the console is not flooded. */
    assert_eq(1, input_route_test_warn_total());

    /* The mouse queue is a DIFFERENT condition: its first overflow is
     * logged as well, so no distinct condition is silenced. */
    memset(&ev, 0, sizeof(ev));
    ev.dx = 1;
    for (i = 0; i < 65; i++) input_route_mouse_event(&ev);
    for (i = 0; i < 20; i++) input_route_mouse_event(&ev);
    assert_eq(1, input_route_test_warn_mouse_overflow());
    assert_eq(2, input_route_test_warn_total());

    /* The stream really was lost: the reader is told so. */
    assert_eq(-EOVERFLOW, input_route_read(kbd, 0, sizeof(buf), buf));
    assert_eq(-EOVERFLOW, input_route_read(mouse, 0, sizeof(events), events));

    g_log_level = saved_level;
}

/* ── The release unlinks the file UNDER the route lock ───────── */

/*
 * A concurrent reader that observes a NULL dev_private gets -EINVAL
 * ("bad argument") instead of the revoke reason, so the route file must
 * be unlinked ATOMICALLY with the revoke: the clear happens while the
 * route lock is still held.
 *
 * The observer runs when a lock is released, so the fixture can look at
 * the file at the exact moment the route lock is given up: the file must
 * already be unlinked there.
 */
static file_t       *release_watched;
static const void   *release_route_lock;
static int          release_saw_route_lock;
static int          release_saw_unlinked;

static void release_lock_observer(const void *lock)
{
    if (lock != release_route_lock)
        return;
    release_saw_route_lock = 1;
    if (release_watched && release_watched->dev_private == NULL)
        release_saw_unlinked = 1;
}

static void test_release_unlinks_the_file_under_the_route_lock(void)
{
    file_t *kbd, *mouse;
    uint64_t generation = 0;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_true(kbd != NULL);
    assert_true(mouse != NULL);
    assert_eq(0, input_route_bind(view(), kbd, mouse, &generation, NULL));
    assert_true(kbd->dev_private != NULL);

    /* Learn the route lock: it is the first lock the route takes. */
    input_route_test_trace_reset();
    {
        uint8_t probe[4];
        (void)input_route_read(kbd, 0, sizeof(probe), probe);
    }
    release_route_lock = input_route_test_trace_first_lock();
    assert_not_null(release_route_lock);

    release_watched = kbd;
    release_saw_route_lock = 0;
    release_saw_unlinked = 0;
    input_route_test_set_lock_release_hook(release_lock_observer);
    input_route_release_file(kbd);
    input_route_test_set_lock_release_hook(NULL);
    release_watched = NULL;

    /* The route lock was given up during the release ... */
    assert_true(release_saw_route_lock);
    /* ... and the file was already unlinked at that moment. */
    assert_true(release_saw_unlinked);
}

/* ── The ACTIVE queues survive a drain-then-refill ──────────── */

static void test_active_queue_survives_drain_and_refill(void)
{
    file_t *kbd, *mouse;
    uint8_t buf[300];
    mouse_event_t events[80];
    mouse_event_t ev;
    int i;

    route_reset();
    kbd = route_open(INPUT_DEVICE_KEYBOARD);
    mouse = route_open(INPUT_DEVICE_MOUSE);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));

    /* Fill, drain half of it, refill: the unread bytes must survive. */
    for (i = 0; i < 100; i++) feed1(0x11 + (uint8_t)(i & 0x0F));
    assert_eq(50, drain_bytes(kbd, buf, 50));
    for (i = 0; i < 50; i++) feed1(0x21 + (uint8_t)(i & 0x0F));

    assert_eq(100, drain_bytes(kbd, buf, sizeof(buf)));
    /* The 50 bytes that were NOT drained come first, in order. */
    for (i = 0; i < 50; i++)
        assert_eq(0x11 + (uint8_t)((50 + i) & 0x0F), buf[i]);
    for (i = 0; i < 50; i++)
        assert_eq(0x21 + (uint8_t)(i & 0x0F), buf[50 + i]);

    /* The same for the mouse queue. */
    memset(&ev, 0, sizeof(ev));
    for (i = 0; i < 40; i++) {
        ev.dx = (int16_t)(i + 1);
        input_route_mouse_event(&ev);
    }
    assert_eq(20, drain_events(mouse, events, 20));
    for (i = 0; i < 20; i++) {
        ev.dx = (int16_t)(100 + i);
        input_route_mouse_event(&ev);
    }
    assert_eq(40, drain_events(mouse, events, 80));
    /* The 20 events that were NOT drained come first, in order. */
    for (i = 0; i < 20; i++)
        assert_eq(21 + i, events[i].dx);
    for (i = 0; i < 20; i++)
        assert_eq(100 + i, events[20 + i].dx);
}

/* Lease teardown must quarantine all held input, including typematic. */
static void test_revoke_isolates_held_input(void)
{
    for (int how = 0; how < 3; how++) {
        route_reset();
        file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
        file_t *mouse = route_open(INPUT_DEVICE_MOUSE);
        file_t *reader = route_open(INPUT_DEVICE_KEYBOARD);
        assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));
        feed1(0x1e);
        mouse_event_t ev = {.buttons = 1};
        input_route_mouse_event(&ev);
        if (how == 0) input_route_unbind(view(), 0, 0);
        else if (how == 1) route_close(kbd);
        else input_route_unbind(view(), 0, EACCES);
        feed1(0x1e);
        feed1(0x9e);
        uint8_t bytes[8];
        assert_eq(0, input_route_read(reader, 0, sizeof(bytes), bytes));
        ev.dx = 7;
        input_route_mouse_event(&ev);
        mouse_event_t out;
        assert_eq(sizeof(out), input_route_test_normal_read_mouse(&out, sizeof(out)));
        assert_eq(0, out.buttons);
        assert_eq(7, out.dx);
        ev.buttons = 0;
        input_route_mouse_event(&ev);
        ev.buttons = 1;
        input_route_mouse_event(&ev);
        mouse_event_t events[2];
        assert_eq(sizeof(events), input_route_test_normal_read_mouse(events, sizeof(events)));
        assert_eq(1, events[1].buttons);
        feed1(0x1e);
        assert_eq(1, input_route_read(reader, 0, sizeof(bytes), bytes));
    }
}

static void test_cross_epoch_modifier_never_reaches_normal_snapshot(void)
{
    route_reset();
    file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
    assert_eq(0, input_route_bind(view(), kbd, NULL, NULL, NULL));
    feed1(0xe0);
    input_route_unbind(view(), 0, 0);
    feed1(0x1d);
    feed1(0x2e);
    assert_eq(0, input_route_test_normal_kbd_modifiers());
    feed1(0xe0); feed1(0x1d); /* isolated typematic */
    feed1(0x2e);
    assert_eq(0, input_route_test_normal_kbd_modifiers());
    feed1(0xe0); feed1(0x9d);
    feed1(0xe0); feed1(0x1d);
    feed1(0x2e);
    assert_eq(INPUT_ROUTE_MOD_CTRL, input_route_test_normal_kbd_modifiers());
}

static void test_left_right_modifiers_are_independent(void)
{
    const uint8_t left[] = {0x2a, 0x1d, 0x38};
    const uint8_t right[] = {0x36, 0x1d, 0x38};
    const uint32_t want[] = {1, 2, 4};
    for (int i = 0; i < 3; i++) {
        for (int reverse = 0; reverse < 2; reverse++) {
            route_reset();
            feed1(left[i]);
            if (i) feed1(0xe0);
            feed1(right[i]);
            if (reverse && i) feed1(0xe0);
            feed1((reverse ? right[i] : left[i]) | 0x80);
            feed1(0x1e);
            assert_eq(want[i], input_route_test_normal_kbd_modifiers());
            if (!reverse && i) feed1(0xe0);
            feed1((reverse ? left[i] : right[i]) | 0x80);
            feed1(0x1e);
            assert_eq(0, input_route_test_normal_kbd_modifiers());
        }
    }
}

static void test_bind_discards_only_leased_normal_backlog(void)
{
    for (int with_mouse = 0; with_mouse < 2; with_mouse++) {
        route_reset();
        file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
        file_t *mouse = route_open(INPUT_DEVICE_MOUSE);
        feed1(0x1e); feed1(0x9e);
        mouse_event_t ev = {.dx = 4};
        input_route_mouse_event(&ev);
        input_route_test_trace_reset();
        assert_eq(0, input_route_bind(view(), kbd, with_mouse ? mouse : NULL, NULL, NULL));
        input_route_unbind(view(), 0, 0);
        uint8_t bytes[8];
        assert_eq(0, input_route_read(kbd, 0, sizeof(bytes), bytes));
        mouse_event_t out;
        assert_eq(with_mouse ? 0 : sizeof(out), input_route_read(mouse, 0, sizeof(out), &out));
        assert_eq(2, input_route_test_trace_first_depth(input_route_test_device_lock()));
        assert_eq(0, input_route_test_trace_mismatch());
        assert_eq(0, input_route_test_trace_depth_now());
    }
}

static void test_bind_wakes_normal_pollers(void)
{
    route_reset();
    file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
    file_t *mouse = route_open(INPUT_DEVICE_MOUSE);
    file_t *other = route_open(INPUT_DEVICE_KEYBOARD);
    poll_table_t kpt, mpt;
    assert_eq(0, poll_table_setup(&kpt, 1));
    assert_eq(0, poll_table_setup(&mpt, 1));
    assert_eq(0, input_route_poll(other, POLLIN, &kpt));
    assert_eq(0, input_route_poll(mouse, POLLIN, &mpt));
    assert_eq(1, kpt.nent);
    assert_eq(1, mpt.nent);
    input_route_test_wake_reset();
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));
    assert_eq(2, input_route_test_wake_count());
    assert_eq(POLLERR, input_route_poll(other, POLLIN, NULL));
    poll_table_destroy(&kpt);
    poll_table_destroy(&mpt);
}

/* A quarantined right modifier must not keep a newly pressed left
 * modifier logically held after the left modifier is released. */
static void test_isolated_modifier_cannot_resurrect_its_other_side(void)
{
    route_reset();
    file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
    assert_eq(0, input_route_bind(view(), kbd, NULL, NULL, NULL));
    feed1(0xe0); feed1(0x1d);
    input_route_unbind(view(), 0, 0);
    feed1(0x1d); feed1(0x9d); feed1(0x2e);
    assert_eq(0, input_route_test_normal_kbd_modifiers());
    feed1(0xe0); feed1(0x1d); feed1(0x2e);
    assert_eq(0, input_route_test_normal_kbd_modifiers());
    feed1(0xe0); feed1(0x9d);
    feed1(0xe0); feed1(0x1d); feed1(0x2e);
    assert_eq(INPUT_ROUTE_MOD_CTRL, input_route_test_normal_kbd_modifiers());
}

static void test_partial_sequence_across_bind_and_revoke_stays_isolated(void)
{
    route_reset();
    file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
    /* A sequence begun by NORMAL cannot become a GUI-owned press. */
    feed1(0xe0);
    assert_eq(0, input_route_bind(view(), kbd, NULL, NULL, NULL));
    feed1(0x1d);
    input_route_unbind(view(), 0, 0);
    feed1(0xe0); feed1(0x1d); feed1(0x2e);
    assert_eq(0, input_route_test_normal_kbd_modifiers());
    /* Release clears quarantine; a fresh press can take effect. */
    feed1(0xe0); feed1(0x9d);
    feed1(0xe0); feed1(0x1d); feed1(0x2e);
    assert_eq(INPUT_ROUTE_MOD_CTRL, input_route_test_normal_kbd_modifiers());
}

static void test_keyboard_only_replacement_preserves_mouse_quarantine(void)
{
    route_reset();
    file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
    file_t *mouse = route_open(INPUT_DEVICE_MOUSE);
    file_t *next_kbd = route_open(INPUT_DEVICE_KEYBOARD);
    assert_eq(0, input_route_bind(view(), kbd, mouse, NULL, NULL));
    mouse_event_t ev = {.buttons = 1};
    input_route_mouse_event(&ev);
    assert_eq(0, input_route_bind(view(), next_kbd, NULL, NULL, NULL));
    ev.dx = 5;
    input_route_mouse_event(&ev);
    mouse_event_t out;
    assert_eq(sizeof(out), input_route_test_normal_read_mouse(&out, sizeof(out)));
    assert_eq(0, out.buttons);
    assert_eq(5, out.dx);
    input_route_unbind(view(), 0, 0);
    ev.dx = 6;
    input_route_mouse_event(&ev);
    assert_eq(sizeof(out), input_route_test_normal_read_mouse(&out, sizeof(out)));
    assert_eq(0, out.buttons);
}

static void test_bind_isolates_held_modifiers_until_fresh_press(void)
{
    const uint8_t codes[] = {0x2a, 0x36, 0x1d, 0x1d, 0x38, 0x38};
    const uint8_t extended[] = {0, 0, 0, 1, 0, 1};
    const uint32_t mods[] = {1, 1, 2, 2, 4, 4};
    for (int i = 0; i < 6; i++) {
        route_reset();
        file_t *kbd = route_open(INPUT_DEVICE_KEYBOARD);
        if (extended[i]) feed1(0xe0);
        feed1(codes[i]);
        uint32_t initial;
        assert_eq(0, input_route_bind(view(), kbd, NULL, NULL, &initial));
        assert_eq(mods[i], initial);
        if (extended[i]) feed1(0xe0);
        feed1(codes[i]); /* held key typematic must not become new DOWN */
        uint8_t buf[8];
        assert_eq(-EAGAIN, input_route_read(kbd, 0, sizeof(buf), buf));
        if (extended[i]) feed1(0xe0);
        feed1(codes[i] | 0x80);
        assert_eq(-EAGAIN, input_route_read(kbd, 0, sizeof(buf), buf));
        if (extended[i]) feed1(0xe0);
        feed1(codes[i]);
        assert_eq(extended[i] ? 2 : 1, input_route_read(kbd, 0, sizeof(buf), buf));
        assert_eq(codes[i], buf[extended[i] ? 1 : 0]);
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_bind_isolates_held_modifiers_until_fresh_press),
    TEST_ENTRY(test_isolated_modifier_cannot_resurrect_its_other_side),
    TEST_ENTRY(test_partial_sequence_across_bind_and_revoke_stays_isolated),
    TEST_ENTRY(test_keyboard_only_replacement_preserves_mouse_quarantine),
    TEST_ENTRY(test_bind_wakes_normal_pollers),
    TEST_ENTRY(test_revoke_isolates_held_input),
    TEST_ENTRY(test_cross_epoch_modifier_never_reaches_normal_snapshot),
    TEST_ENTRY(test_left_right_modifiers_are_independent),
    TEST_ENTRY(test_bind_discards_only_leased_normal_backlog),
    TEST_ENTRY(test_normal_path_before_bind),
    TEST_ENTRY(test_lease_owner_reads_others_busy),
    TEST_ENTRY(test_empty_active_queue_is_eagain),
    TEST_ENTRY(test_dup_revokes_only_on_final_close),
    TEST_ENTRY(test_close_any_bound_file_revokes_lease),
    TEST_ENTRY(test_recycled_address_does_not_release_new_lease),
    TEST_ENTRY(test_keyboard_active_queue_overflow),
    TEST_ENTRY(test_mouse_active_queue_overflow),
    TEST_ENTRY(test_active_queue_survives_drain_and_refill),
    TEST_ENTRY(test_boundary_sequences_never_publish_half),
    TEST_ENTRY(test_cross_epoch_make_is_isolated_until_break),
    TEST_ENTRY(test_cross_epoch_make_reports_desync),
    TEST_ENTRY(test_desync_is_cleared_at_bind),
    TEST_ENTRY(test_bind_with_held_key_produces_no_new_down),
    TEST_ENTRY(test_caps_typematic_does_not_flip),
    TEST_ENTRY(test_mouse_button_isolation_keeps_movement),
    TEST_ENTRY(test_revoke_resets_the_transient_modifier_state),
    TEST_ENTRY(test_release_unlinks_the_file_under_the_route_lock),
    TEST_ENTRY(test_overflow_warning_is_rate_limited_but_not_silent),
    TEST_ENTRY(test_lock_order_is_route_then_normal_queue),
    TEST_ENTRY(test_no_lost_wakeup_at_register_boundary),
    TEST_ENTRY(test_unbind_generation_rules),
    TEST_ENTRY(test_revoked_lease_reports_the_reason),
TEST_LIST_END

int main(void)
{
    printf("=== Test Runner ===\n");
    for (int i = 0; i < __test_table_size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }

    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed > 0 ? 1 : 0;
}
