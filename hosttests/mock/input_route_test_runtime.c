/*
 * input_route_test_runtime.c — the host runtime behind
 * input_route_test_runtime.h.
 *
 * Everything here is a stand-in for a kernel layer that the route
 * fixture cannot host-compile (the arch spinlock, the file
 * lifecycle, the poll table, the log).  The parts that carry the
 * route's CONTRACT are not stubs: the spinlock records the lock
 * order, the file model reproduces the refcount protocol, and the
 * NORMAL sink publishes into a real queue under its own lock.
 */

#include "input_route_test_runtime.h"

#include <driver/input_route.h>
#include <driver/input_ps2.h>
#include <uapi/mouse.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <log/log.h>

/* ── The recorded lock trace ─────────────────────────────────── */

#define TRACE_MAX 512

static input_route_test_event_t trace[TRACE_MAX];
static int         trace_count;
static int         trace_overflow;
static int         trace_mismatch;
static const void *lock_stack[TRACE_MAX];
static int         lock_depth;

/* ── The lock-release observer ─────────────────────────────────
 * The hook runs when a lock is RELEASED, with the lock object, so a
 * fixture can look at the world at that exact moment.  It is how the
 * "the route file is unlinked UNDER the route lock" contract is
 * pinned: at the route lock's release the file's dev_private must
 * already be NULL.
 */
static void (*lock_release_hook)(const void *lock);

void input_route_test_set_lock_release_hook(void (*hook)(const void *lock))
{
    lock_release_hook = hook;
}

void input_route_test_lock_acquired(const void *lock)
{
    int i;

    /* A re-entrant acquisition of an already held lock: recorded, not
     * rejected — the fixture injects a publish from inside poll_wait
     * on purpose. */
    for (i = 0; i < lock_depth; i++) {
        if (lock_stack[i] == lock) {
            if (trace_count < TRACE_MAX) {
                trace[trace_count].lock = lock;
                trace[trace_count].acquired = 1;
                trace[trace_count].depth = lock_depth + 1;
                trace_count++;
            }
            if (lock_depth < TRACE_MAX) lock_stack[lock_depth] = lock;
            lock_depth++;
            return;
        }
    }
    if (lock_depth < TRACE_MAX) lock_stack[lock_depth] = lock;
    lock_depth++;
    if (trace_count < TRACE_MAX) {
        trace[trace_count].lock = lock;
        trace[trace_count].acquired = 1;
        trace[trace_count].depth = lock_depth;
        trace_count++;
    } else {
        trace_overflow = 1;
    }
}

void input_route_test_lock_released(const void *lock)
{
    /* The observer runs BEFORE the depth bookkeeping, so the hook sees
     * the lock as it is being given up. */
    if (lock_release_hook)
        lock_release_hook(lock);
    if (lock_depth <= 0) {
        trace_mismatch = 1;
        return;
    }
    lock_depth--;
    if (lock_stack[lock_depth] != lock) trace_mismatch = 1;
    if (trace_count < TRACE_MAX) {
        trace[trace_count].lock = lock;
        trace[trace_count].acquired = 0;
        trace[trace_count].depth = lock_depth;
        trace_count++;
    } else {
        trace_overflow = 1;
    }
}

void input_route_test_trace_reset(void)
{
    trace_count = 0;
    trace_overflow = 0;
    trace_mismatch = 0;
    lock_depth = 0;
}

int input_route_test_trace_count(void)
{
    return trace_count;
}

const input_route_test_event_t *input_route_test_trace_event(int index)
{
    if (index < 0 || index >= trace_count) return NULL;
    return &trace[index];
}

int input_route_test_trace_first_depth(const void *lock)
{
    int i;
    for (i = 0; i < trace_count; i++) {
        if (trace[i].lock == lock && trace[i].acquired) return trace[i].depth;
    }
    return -1;
}

int input_route_test_trace_reentry_depth(const void *lock)
{
    int i;
    int seen = 0;
    for (i = 0; i < trace_count; i++) {
        if (trace[i].lock != lock) continue;
        if (!trace[i].acquired) { seen = 0; continue; }
        if (seen) return trace[i].depth;
        seen = 1;
    }
    return -1;
}

int input_route_test_trace_mismatch(void)
{
    return trace_mismatch || trace_overflow;
}

int input_route_test_trace_depth_now(void)
{
    return lock_depth;
}

const void *input_route_test_trace_first_lock(void)
{
    if (trace_count == 0) return NULL;
    return trace[0].lock;
}

/* ── The heap ───────────────────────────────────────────────── */

#define ALLOC_MAX 64

static void *alloc_live[ALLOC_MAX];
static int   alloc_live_count;
static int   alloc_double_free;

void *kmalloc(size_t size)
{
    void *p = calloc(1, size ? size : 1);
    if (!p) return NULL;
    if (alloc_live_count < ALLOC_MAX) alloc_live[alloc_live_count++] = p;
    return p;
}

size_t kfree(void *ptr)
{
    int i;
    if (!ptr) return 0;
    for (i = 0; i < alloc_live_count; i++) {
        if (alloc_live[i] == ptr) {
            alloc_live[i] = alloc_live[--alloc_live_count];
            free(ptr);
            return 0;
        }
    }
    /* A free of an address that is not live: a double free or a free of
     * a foreign pointer.  The fixture asserts this never happens. */
    alloc_double_free = 1;
    return 0;
}

int input_route_test_alloc_double_free(void);
int input_route_test_alloc_double_free(void) { return alloc_double_free; }

/* ── The host file lifecycle ────────────────────────────────── */

#define MOCK_MAX_FILES 16

static file_t file_pool[MOCK_MAX_FILES];
static int    file_used[MOCK_MAX_FILES];
static void (*release_hook)(file_t *f);
static int    file_double_free;

void input_route_test_set_release_hook(void (*hook)(file_t *f))
{
    release_hook = hook;
}

file_t *file_alloc(void)
{
    int i;
    for (i = 0; i < MOCK_MAX_FILES; i++) {
        if (file_used[i]) continue;
        memset(&file_pool[i], 0, sizeof(file_pool[i]));
        file_pool[i].refcount = 1;
        file_used[i] = 1;
        return &file_pool[i];
    }
    return NULL;
}

void file_free(file_t *f)
{
    int i;
    if (!f) return;
    for (i = 0; i < MOCK_MAX_FILES; i++) {
        if (&file_pool[i] != f) continue;
        if (!file_used[i]) { file_double_free = 1; return; }
        break;
    }
    if (i == MOCK_MAX_FILES) { file_double_free = 1; return; }

    /* The devfs_release_file → ops->release_file chain: the device
     * release hook runs exactly once, before the slot is recycled. */
    if (release_hook) release_hook(f);
    file_used[i] = 0;
}

void file_get(file_t *f)
{
    if (!f) return;
    f->refcount++;
}

void file_put(file_t *f)
{
    if (!f) return;
    if (f->refcount > 0) f->refcount--;
    if (f->refcount == 0) file_free(f);
}

int input_route_test_file_double_free(void);
int input_route_test_file_double_free(void) { return file_double_free; }

/* ── The NORMAL queue the mock drivers publish into ─────────── */

#define NORMAL_KBD_BYTES 256
#define NORMAL_MOUSE_SLOTS 16

static struct {
    spinlock_T    lock;          /* the "device lock" of the NORMAL queue */
    uint8_t       bytes[NORMAL_KBD_BYTES];
    unsigned      head, tail, count;
    mouse_event_t events[NORMAL_MOUSE_SLOTS];
    unsigned      ehead, etail, ecount;
    unsigned      frames;
    uint32_t      last_mods;     /* the modifier snapshot of the last frame */
} normal;

const void *input_route_test_device_lock(void)
{
    return &normal.lock;
}

void input_route_test_normal_reset(void)
{
    memset(normal.bytes, 0, sizeof(normal.bytes));
    memset(normal.events, 0, sizeof(normal.events));
    normal.head = normal.tail = normal.count = 0;
    normal.ehead = normal.etail = normal.ecount = 0;
    normal.frames = 0;
    normal.last_mods = 0;
}

void input_route_test_normal_push_bytes(const uint8_t *bytes, size_t len)
{
    size_t i;
    unsigned long flags = spin_lock_irqsave(&normal.lock);
    for (i = 0; i < len; i++) {
        if (normal.count >= NORMAL_KBD_BYTES - 1) break;
        normal.bytes[(normal.head + normal.count) % NORMAL_KBD_BYTES] = bytes[i];
        normal.count++;
    }
    spin_unlock_irqrestore(&normal.lock, flags);
}

void input_route_test_normal_push_event(const mouse_event_t *event)
{
    unsigned long flags = spin_lock_irqsave(&normal.lock);
    if (normal.ecount < NORMAL_MOUSE_SLOTS) {
        normal.events[(normal.ehead + normal.ecount) % NORMAL_MOUSE_SLOTS] = *event;
        normal.ecount++;
    }
    spin_unlock_irqrestore(&normal.lock, flags);
}

int input_route_test_normal_read_kbd(uint8_t *buf, size_t size)
{
    size_t n = 0;
    unsigned long flags = spin_lock_irqsave(&normal.lock);
    while (n < size && normal.count > 0) {
        buf[n++] = normal.bytes[normal.tail];
        normal.tail = (normal.tail + 1) % NORMAL_KBD_BYTES;
        normal.count--;
    }
    spin_unlock_irqrestore(&normal.lock, flags);
    return (int)n;
}

int input_route_test_normal_read_mouse(void *buf, size_t size)
{
    size_t n = 0;
    unsigned long flags = spin_lock_irqsave(&normal.lock);
    while ((n + 1) * sizeof(mouse_event_t) <= size && normal.ecount > 0) {
        ((mouse_event_t *)buf)[n++] = normal.events[normal.etail];
        normal.etail = (normal.etail + 1) % NORMAL_MOUSE_SLOTS;
        normal.ecount--;
    }
    spin_unlock_irqrestore(&normal.lock, flags);
    return (int)(n * sizeof(mouse_event_t));
}

unsigned input_route_test_normal_kbd_bytes(void) { return normal.count; }
unsigned input_route_test_normal_mouse_events(void) { return normal.ecount; }
unsigned input_route_test_normal_frames(void) { return normal.frames; }

/* The modifier snapshot the NORMAL sink was last handed.  The NORMAL
 * sink USED to discard it, which left the revoke-time transient
 * modifier reset completely unpinned: deleting the reset in the route
 * kept the whole host suite green. */
uint32_t input_route_test_normal_kbd_modifiers(void) { return normal.last_mods; }

/* ── The NORMAL sink handed to the route ────────────────────── */

static void test_publish_frame(const input_ps2_frame_t *frame, uint32_t modifiers)
{
    unsigned long flags = spin_lock_irqsave(&normal.lock);
    normal.frames++;
    normal.last_mods = modifiers;
    spin_unlock_irqrestore(&normal.lock, flags);
    input_route_test_normal_push_bytes(frame->bytes, frame->length);
}

static int test_read_kbd(uint64_t offset, uint64_t size, void *buf)
{
    (void)offset;
    return input_route_test_normal_read_kbd((uint8_t *)buf, (size_t)size);
}

static uint32_t test_poll_kbd(uint32_t requested, poll_table_t *pt)
{
    (void)requested;
    (void)pt;
    return normal.count ? (POLLIN | POLLRDNORM) : 0;
}

static void test_publish_mouse(const mouse_event_t *event)
{
    input_route_test_normal_push_event(event);
}

static int test_read_mouse(uint64_t offset, uint64_t size, void *buf)
{
    (void)offset;
    return input_route_test_normal_read_mouse(buf, (size_t)size);
}

static uint32_t test_poll_mouse(uint32_t requested, poll_table_t *pt)
{
    (void)requested;
    (void)pt;
    return normal.ecount ? (POLLIN | POLLRDNORM) : 0;
}

const input_route_kbd_sink_t input_route_test_kbd_sink = {
    .publish = test_publish_frame,
    .read    = test_read_kbd,
    .poll    = test_poll_kbd,
};

const input_route_mouse_sink_t input_route_test_mouse_sink = {
    .publish = test_publish_mouse,
    .read    = test_read_mouse,
    .poll    = test_poll_mouse,
};

/* ── The poll table stand-ins ───────────────────────────────── */

static void (*poll_wait_hook)(void);
static unsigned wake_count;

void input_route_test_set_poll_wait_hook(void (*hook)(void))
{
    poll_wait_hook = hook;
}

unsigned input_route_test_wake_count(void) { return wake_count; }
void input_route_test_wake_reset(void) { wake_count = 0; }

void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock)
{
    poll_wait_entry_t *e;

    if (!pt || !pt->entries || !poll_list || !fd_lock) return;
    if (pt->nent < 0 || pt->nent >= pt->max_entries || pt->triggered) return;

    /* The injection point: a publish that lands between the readiness
     * check and the registration must not be lost. */
    if (poll_wait_hook) poll_wait_hook();

    e = &pt->entries[pt->nent++];
    e->poll_wq = &pt->wq;
    e->fd_lock = fd_lock;
    list_add_to_before(poll_list, &e->node);
}

void poll_table_init(poll_table_t *pt)
{
    if (!pt) return;
    pt->nent = 0;
    pt->triggered = false;
}

int poll_table_setup(poll_table_t *pt, int max_entries)
{
    int i;

    if (!pt) return -EINVAL;
    pt->entries = NULL;
    pt->max_entries = 0;
    pt->nent = 0;
    pt->triggered = false;
    if (max_entries <= 0) return -EINVAL;

    wait_queue_init(&pt->wq);
    pt->entries = kmalloc((size_t)max_entries * sizeof(poll_wait_entry_t));
    if (!pt->entries) return -ENOMEM;
    pt->max_entries = max_entries;
    for (i = 0; i < max_entries; i++) list_init(&pt->entries[i].node);
    return 0;
}

void poll_table_destroy(poll_table_t *pt)
{
    if (!pt) return;
    if (pt->entries) kfree(pt->entries);
    pt->entries = NULL;
    pt->max_entries = 0;
    pt->nent = 0;
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wake_count++;
}

void wait_queue_wake_one(wait_queue_t *wq)
{
    (void)wq;
    wake_count++;
}

void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}

/* ── The log stand-ins ──────────────────────────────────────── */

/* WEAK: the GUI-2 Task 4 fixture links this runtime together with the
 * fb host runtime (hosttests/mock/fb_resolution_runtime.c), which
 * defines the same log-level knob.  A weak definition keeps the route
 * fixture self-contained while letting the other runtime win when both
 * are linked into one binary. */
__attribute__((weak)) int g_log_level = LOG_ERR;

void _log_err_impl(const char *fmt, ...) { (void)fmt; }

/* The warnings are COUNTED, not dropped: the route's console traffic is
 * part of its contract (the overflow warning must not flood the serial
 * console, but the FIRST occurrence of each condition must still be
 * logged), so the fixture needs to see how often each one fires. */
static unsigned warn_total;
static unsigned warn_kbd_overflow;
static unsigned warn_mouse_overflow;

void _log_warn_impl(const char *fmt, ...)
{
    warn_total++;
    if (fmt && strstr(fmt, "keyboard queue overflow"))
        warn_kbd_overflow++;
    else if (fmt && strstr(fmt, "mouse queue overflow"))
        warn_mouse_overflow++;
}

void _log_info_impl(const char *fmt, ...) { (void)fmt; }

unsigned input_route_test_warn_total(void) { return warn_total; }
unsigned input_route_test_warn_kbd_overflow(void) { return warn_kbd_overflow; }
unsigned input_route_test_warn_mouse_overflow(void) { return warn_mouse_overflow; }

void input_route_test_warn_reset(void)
{
    warn_total = 0;
    warn_kbd_overflow = 0;
    warn_mouse_overflow = 0;
}
