/*
 * input_route_test_runtime.h — host-only runtime for the input-route
 * fixture (GUI-2 plan Task 3).
 *
 * The route's contract is a strict lock order (route → NORMAL
 * queue/poll) and an ATOMIC "authorize + dequeue + register waiter"
 * window.  A no-op mock spinlock cannot show either of them, so this
 * runtime provides a RECORDING spinlock: every acquisition and release
 * is captured together with the nesting depth, an unbalanced release is
 * reported, and a re-entrant acquisition is counted.  The fixture
 * asserts on the recorded order, not on a return value.
 *
 * It also provides:
 *   - the host stand-in for the kernel file lifecycle (file_alloc /
 *     file_get / file_put / file_free with the devfs release hook), so
 *     "the FINAL close revokes" can be observed;
 *   - the NORMAL-path sink the drivers publish into (a byte ring for
 *     the keyboard, an event ring for the mouse) guarded by its own
 *     "device lock", so the recorded order shows route → device;
 *   - the poll_wait / wait_queue_wake_all stand-ins, including the
 *     hook that injects a publish at the check-empty → register-waiter
 *     boundary.
 */

#ifndef OS01_INPUT_ROUTE_TEST_RUNTIME_H
#define OS01_INPUT_ROUTE_TEST_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <list.h>

/* ── The recording spinlock ──────────────────────────────────────
 * The lock is not a no-op: it keeps a stack of the held locks so the
 * fixture can observe WHICH locks are held and IN WHICH ORDER, and it
 * reports an unbalanced release.  The stack is deliberately
 * re-entrant-tolerant (the fixture injects a publish from inside
 * poll_wait, which re-enters the route lock) — the recorded nesting
 * is what the assertions look at.
 */
typedef struct {
    volatile unsigned long lock;
} spinlock_T;

void input_route_test_lock_acquired(const void *lock);
void input_route_test_lock_released(const void *lock);

static inline void spin_init(spinlock_T *l) { l->lock = 1UL; }

static inline unsigned long spin_lock_irqsave(spinlock_T *l)
{
    input_route_test_lock_acquired(l);
    return 0UL;
}

static inline void spin_unlock_irqrestore(spinlock_T *l, unsigned long flags)
{
    (void)flags;
    input_route_test_lock_released(l);
}

static inline void spin_lock(spinlock_T *l)
{
    input_route_test_lock_acquired(l);
}

static inline void spin_unlock(spinlock_T *l)
{
    input_route_test_lock_released(l);
}

/* ── The recorded lock trace ─────────────────────────────────── */

#include <fs/file.h>
#include <fs/poll.h>
#include <driver/input_route.h>

typedef struct {
    const void *lock;      /* the lock object */
    int         acquired;  /* 1 = acquired, 0 = released */
    int         depth;     /* the lock-stack depth AFTER the event */
} input_route_test_event_t;

void input_route_test_trace_reset(void);
int  input_route_test_trace_count(void);
const input_route_test_event_t *input_route_test_trace_event(int index);
/* The depth at which `lock` was acquired the first time, or -1. */
int  input_route_test_trace_first_depth(const void *lock);
/* The depth at which `lock` was RE-acquired while already held, or -1. */
int  input_route_test_trace_reentry_depth(const void *lock);
int  input_route_test_trace_mismatch(void);
int  input_route_test_trace_depth_now(void);
/* The first lock object acquired since the last trace reset. */
const void *input_route_test_trace_first_lock(void);

/* ── The host stand-in for the kernel file lifecycle ───────────
 * file_put() reaches file_free() only when the last reference is
 * dropped, and file_free() runs the device release hook exactly once
 * — the devfs_release_file → ops->release_file chain.
 */
void input_route_test_set_release_hook(void (*hook)(file_t *f));
/* A free of an address that is not live (a double free) was seen. */
int  input_route_test_alloc_double_free(void);
int  input_route_test_file_double_free(void);

/* ── The NORMAL-path sink ──────────────────────────────────────
 * The mock drivers publish into these queues.  Their own lock is the
 * "device lock" of the recorded order.
 */
extern const input_route_kbd_sink_t   input_route_test_kbd_sink;
extern const input_route_mouse_sink_t input_route_test_mouse_sink;

void input_route_test_normal_reset(void);
void input_route_test_normal_push_bytes(const uint8_t *bytes, size_t len);
void input_route_test_normal_push_event(const mouse_event_t *event);
int  input_route_test_normal_read_kbd(uint8_t *buf, size_t size);
int  input_route_test_normal_read_mouse(void *buf, size_t size);
unsigned input_route_test_normal_kbd_bytes(void);
unsigned input_route_test_normal_mouse_events(void);
unsigned input_route_test_normal_frames(void);
const void *input_route_test_device_lock(void);

/* ── The poll_wait injection point ─────────────────────────────
 * The hook runs INSIDE poll_wait, before the waiter is registered —
 * exactly the window between the readiness check and the
 * registration.  A publish injected there must not be lost.
 */
void input_route_test_set_poll_wait_hook(void (*hook)(void));
/* The number of wait_queue_wake_all() calls the mock performed. */
unsigned input_route_test_wake_count(void);
void input_route_test_wake_reset(void);

#endif /* OS01_INPUT_ROUTE_TEST_RUNTIME_H */
