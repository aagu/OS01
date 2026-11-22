#include <stdbool.h>
#include <stdint.h>
#include <driver/mouse.h>
#include <driver/input_route.h>
#include <percpu/percpu.h>
#include <errno.h>
#include <fs/devfs.h>
#include <kernel.h>

/* The i8042 callback runs under the controller lock. It performs no port
 * I/O or sleep, and only takes the event lock after the i8042 lock.
 * Neither event read nor poll acquires the i8042 lock. */
#define MOUSE_RING_CAPACITY 64
static mouse_event_t events[MOUSE_RING_CAPACITY];
static unsigned event_head, event_count;
static uint64_t dropped_events;
static spinlock_T event_lock;
static list_t event_poll;

/* The NORMAL-path publication: the legacy /dev/mouse event ring.
 *
 * The input route calls this while it holds its own lock, so the event
 * lock below is always taken INSIDE the route lock (lock order route
 * → NORMAL queue/poll).  The mouse driver itself never takes the event
 * lock before the route: input_route_mouse_event() is called FIRST,
 * so the forbidden inversion (event lock → route lock) cannot happen.
 */
static void mouse_publish_normal_event(const mouse_event_t *event)
{
    uint64_t flags = spin_lock_irqsave(&event_lock);
    if (event_count == MOUSE_RING_CAPACITY) {
        event_head = (event_head + 1) % MOUSE_RING_CAPACITY;
        event_count--;
        dropped_events++;
    }
    events[(event_head + event_count) % MOUSE_RING_CAPACITY] = *event;
    event_count++;
    /* Removing each entry under its fd_lock is the poll cleanup protocol.
     * Wake before releasing it so the poll table cannot be freed in between. */
    while (!list_is_empty(&event_poll)) {
        list_t *node = event_poll.next;
        list_del_init(node);
        poll_wait_entry_t *entry = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(entry->poll_wq);
    }
    spin_unlock_irqrestore(&event_lock, flags);
    if (num_cpus != 0)
        __atomic_store_n(&this_cpu()->need_resched, 1, __ATOMIC_RELEASE);
}

int mouse_devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (size < sizeof(mouse_event_t))
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&event_lock);
    if (!event_count) {
        spin_unlock_irqrestore(&event_lock, flags);
        return -EAGAIN;
    }
    uint64_t requested = size / sizeof(mouse_event_t);
    unsigned n = requested > event_count ? event_count : (unsigned)requested;
    for (unsigned i = 0; i < n; i++) {
        ((mouse_event_t *)buffer)[i] = events[event_head];
        event_head = (event_head + 1) % MOUSE_RING_CAPACITY;
    }
    event_count -= n;
    spin_unlock_irqrestore(&event_lock, flags);
    return (int)(n * sizeof(mouse_event_t));
}

uint32_t mouse_poll_dev(void *priv, uint32_t requested, poll_table_t *pt)
{
    (void)priv;
    uint64_t flags = spin_lock_irqsave(&event_lock);
    uint32_t mask = 0;
    if (event_count)
        mask = POLLIN | POLLRDNORM;
    else if (poll_requested_read(requested) && pt && !pt->triggered)
        poll_wait(pt, &event_poll, &event_lock);
    spin_unlock_irqrestore(&event_lock, flags);
    return mask;
}

uint64_t mouse_dropped_events(void)
{
    uint64_t flags = spin_lock_irqsave(&event_lock);
    uint64_t count = dropped_events;
    spin_unlock_irqrestore(&event_lock, flags);
    return count;
}


/* The NORMAL-path sink the input route publishes into. */
static int mouse_route_read(uint64_t offset, uint64_t size, void *buf)
{
    return mouse_devfs_read(NULL, offset, size, buf);
}

static uint32_t mouse_route_poll(uint32_t requested, poll_table_t *pt)
{
    return mouse_poll_dev(NULL, requested, pt);
}

/* route.lock -> event_lock, matching NORMAL publish/read/poll. */
static void mouse_reset_normal(void)
{
    uint64_t flags = spin_lock_irqsave(&event_lock);
    event_head = event_count = 0;
    while (!list_is_empty(&event_poll)) {
        list_t *node = event_poll.next;
        list_del_init(node);
        poll_wait_entry_t *entry = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(entry->poll_wq);
    }
    spin_unlock_irqrestore(&event_lock, flags);
}

static const input_route_mouse_sink_t mouse_route_sink = {
    .publish = mouse_publish_normal_event,
    .reset   = mouse_reset_normal,
    .read    = mouse_route_read,
    .poll    = mouse_route_poll,
};

/* The device open hook: every /dev/mouse open becomes a route
 * endpoint, so the lease can tell the two open file descriptions
 * apart. */
static int mouse_open(const char *name, struct file **out_file)
{
    (void)name;
    return input_route_open(INPUT_DEVICE_MOUSE, out_file);
}

/* The device operations table.  The file-aware hooks carry the lease:
 * the read/poll hooks honour it and the release hook is the final
 * close that revokes it. */
const struct devfs_ops mouse_ops = {
    .open         = mouse_open,
    .read         = mouse_devfs_read,
    .poll         = mouse_poll_dev,
    .release_file = input_route_release_file,
    .read_file    = input_route_read,
    .poll_file    = input_route_poll,
};

static bool mouse_input_initialized;

void mouse_input_init(void)
{
    if (mouse_input_initialized)
        return;

    spin_init(&event_lock);
    list_init(&event_poll);
    event_head = event_count = 0;
    dropped_events = 0;
    /* The input route publishes the mouse events into the NORMAL ring
     * until a lease takes them over. */
    input_route_set_normal_sinks(NULL, &mouse_route_sink);
    mouse_input_initialized = true;
}

#ifdef OS01_HOST_TEST
void mouse__test_reset(void)
{
    mouse_input_initialized = false;
}
#endif


