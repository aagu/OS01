#ifndef __INPUT_ROUTE_H__
#define __INPUT_ROUTE_H__

#include <stdint.h>
#include <driver/input_ps2.h>
#include <uapi/mouse.h>

/* ═══════════════════════════════════════════════════════════════
 *  Input route — the exclusive physical-input lease
 *
 *  The route sits between the PS/2 drivers and every reader of the
 *  keyboard / mouse bytes.  It owns:
 *
 *    - the PHYSICAL key state (which keys are really held) and the
 *      Caps Lock latch — the single authority, so the Caps state can
 *      never be flipped twice;
 *    - the LEASE: one live lease at a time, owned by one display
 *      view, that makes the physical input EXCLUSIVE.  While the
 *      lease is live the owner's file reads the ACTIVE queue and
 *      every other file on the device is shut out (EBUSY / POLLERR),
 *      so no other reader can steal the bytes and the keyboard stops
 *      leaking into the console TTY;
 *    - the ROUTING decision: a COMPLETE scan sequence (never half of
 *      one) is published either to the legacy NORMAL path or to the
 *      leased ACTIVE path.
 *
 *  Locking: the route's own lock protects the lease, the physical and
 *  isolation bitmaps, the framer, the ACTIVE queues and the endpoint
 *  records.  The lock order is route → NORMAL queue/poll: the route
 *  never takes the i8042 lock, the display state lock or the display
 *  mutex, and the NORMAL read/poll authorises, dequeues and registers
 *  its waiter inside the route's own critical section.
 *
 *  The per-file state stays private to input_route.c: this header
 *  exposes the entry points and the NORMAL-path sink the drivers
 *  register, nothing else.
 * ═══════════════════════════════════════════════════════════════ */

/* The device a route file belongs to. */
typedef enum {
    INPUT_DEVICE_KEYBOARD = 0,
    INPUT_DEVICE_MOUSE = 1,
} input_device_t;

/* The logical modifier bits of the snapshot route hands out.  The
 * momentary modifiers (SHIFT / CTRL / ALT) describe what is held right
 * now; CAPS is the latched Caps Lock state, which survives a lease
 * change because it is a toggle, not a held key. */
#define INPUT_ROUTE_MOD_SHIFT 0x1u
#define INPUT_ROUTE_MOD_CTRL  0x2u
#define INPUT_ROUTE_MOD_ALT   0x4u
#define INPUT_ROUTE_MOD_CAPS  0x8u

struct file;
struct poll_table;

/* ── The NORMAL-path sink ───────────────────────────────────────
 * The drivers publish into the NORMAL path and drain it.  The route
 * calls the sink while it holds its own lock, so the sink's own lock
 * is always taken INSIDE the route lock (lock order route → NORMAL
 * queue/poll).
 *
 * `publish` receives the COMPLETE frame and the logical modifier
 * snapshot route computed for it.  The NORMAL translator uses that
 * snapshot and never flips the Caps state a second time — route is
 * the only authority for the Caps latch.
 * `reset` runs under the route lock at bind, drains only this device's
 * raw NORMAL queue and wakes its old pollers under the device lock.
 * It must preserve data already delivered to the TTY/PTY.
 */
typedef struct {
    void     (*publish)(const input_ps2_frame_t *frame, uint32_t modifiers);
    int      (*read)(uint64_t offset, uint64_t size, void *buf);
    uint32_t (*poll)(uint32_t requested, struct poll_table *pt);
    void     (*reset)(void); /* drain raw backlog and wake old NORMAL pollers */
} input_route_kbd_sink_t;

typedef struct {
    void     (*publish)(const mouse_event_t *event);
    int      (*read)(uint64_t offset, uint64_t size, void *buf);
    uint32_t (*poll)(uint32_t requested, struct poll_table *pt);
    void     (*reset)(void); /* drain raw backlog and wake old NORMAL pollers */
} input_route_mouse_sink_t;

/* Register the NORMAL-path sinks.  A NULL argument leaves that slot
 * unchanged, so each driver registers only its own device. */
void input_route_set_normal_sinks(const input_route_kbd_sink_t *kbd,
                                  const input_route_mouse_sink_t *mouse);

/* ── Lifecycle ─────────────────────────────────────────────────── */

/* Initialise the route.  Called once by the keyboard driver before it
 * installs the byte consumer and enables IRQ1.  The whole state is
 * zero-initialised first: the framer's per-sequence token is not
 * cleared by input_ps2_frame_reset(), and a stale token would make
 * the next multi-byte sequence fail the accumulator check. */
void input_route_init(void);

/* Open a route file for `device`.  The file is an FD_DEV file whose
 * dev_private points at the endpoint state; the caller (the device
 * open hook) attaches the node. */
int  input_route_open(input_device_t device, struct file **out);

/* Release a route file.  Runs under the route lock: the endpoint is
 * revoked and unlinked first, the lock is dropped, and only then is
 * the endpoint freed. */
void input_route_release_file(struct file *f);

/* ── The lease ─────────────────────────────────────────────────── */

/* Grant the lease to `view`.  The keyboard and mouse files must be
 * route endpoints of the matching device.  A live lease is replaced:
 * the previous owner loses it.
 *
 * A NULL `mouse` leases the KEYBOARD alone: the mouse device then stays
 * on the NORMAL path and is not owned by the lease.
 *
 * `generation` receives the new lease generation (non-zero and
 * monotonically increasing) and `initial_modifiers` the modifier
 * snapshot the new consumer starts from. */
int  input_route_bind(const void *view, struct file *kbd, struct file *mouse,
                      uint64_t *generation, uint32_t *initial_modifiers);

/* Release the lease.  `generation` must match the live lease exactly
 * when it is non-zero; zero means the current view's explicit
 * release.  `reason` is 0 for an explicit cleanup and a revoke reason
 * (EACCES / ESTALE / EIO) when the lease is taken away.  A cleanup
 * for another view or another generation never touches the lease. */
void input_route_unbind(const void *view, uint64_t generation, int reason);

/* The live lease, if any: `view` receives the lease's owner and
 * `generation` its generation; the return value is 1 when a lease is
 * live and 0 when there is none.
 *
 * The route revokes the lease on its own — the final close of a bound
 * file revokes it — and that revoke happens INSIDE the route.  This
 * query is how a caller learns about it: the two answers to "is there
 * a lease?" must never diverge.
 *
 * IRQ-safe: it takes only the route lock and never a display lock, so
 * it can be called under the display lock (the lock order is
 * display → route) without inverting anything. */
int  input_route_lease_live(const void **view, uint64_t *generation);

/* ── The file-aware device entry points ────────────────────────── */

/* Read from a route file.  Before the lease the device is NORMAL and
 * the file drains the NORMAL queue; while the lease is live only the
 * owner's file may read (the ACTIVE queue), every other file gets
 * EBUSY.
 *
 * Two ACTIVE-path errors are reported out of band, so the byte stream
 * itself stays raw Set 1 payload:
 *   -EOVERFLOW  the ACTIVE stream is not trustworthy: either a sequence
 *               did not fit the ACTIVE queue (the stream lost data) or a
 *               sequence crossed an epoch boundary and was swallowed (the
 *               consumer's KEY STATE may have diverged from the physical
 *               one).  Sticky: the read AND the poll keep reporting it
 *               for the whole lease.  The consumer has to give the lease
 *               up and take a new one; a NEW lease starts from a
 *               known-consistent state and clears the flag.
 *
 * The spec §4.2 state table names EOVERFLOW for the DESYNC state and
 * says the prohibition continues until the app cleans up or the file is
 * finally closed.  A DEAD endpoint (a revoked lease) reports its
 * recorded revoke reason instead (EACCES / ESTALE / EIO) and stays dead
 * for good, so the two are told apart by the retry. */
int  input_route_read(struct file *f, uint64_t offset, uint64_t size,
                      void *buf);

/* Poll a route file.  The readiness check, the waiter registration
 * and the authorisation all happen in the route's own critical
 * section, so a publish that races the registration cannot be lost.
 * POLLERR reports the two ACTIVE-path errors above (the read tells
 * the consumer which one it is). */
uint32_t input_route_poll(struct file *f, uint32_t requested,
                           struct poll_table *pt);

/* ── The producer entry points ─────────────────────────────────── */

/* Feed one raw PS/2 byte.  The route frames it and publishes a
 * COMPLETE sequence to the NORMAL or the ACTIVE path. */
void input_route_keyboard_byte(uint8_t byte);

/* Feed one mouse event.  The route decides NORMAL or ACTIVE; the
 * mouse driver must call this BEFORE it takes its own event lock. */
void input_route_mouse_event(const mouse_event_t *event);

#ifdef OS01_HOST_TEST
/* Host-only: the DESYNC flag's value.  The flag has NO other observable
 * (only the owner of a LIVE lease can see it), so the fixture needs this
 * to pin WHERE it is cleared: at bind — not at revoke.  The production
 * build does not compile it. */
int input_route__test_desync_active(void);
#endif

#endif /* __INPUT_ROUTE_H__ */
