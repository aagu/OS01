#include <stdbool.h>
#include <stdint.h>
#include <driver/i8042.h>
#include <driver/mouse.h>
#include <driver/mouse_proto.h>
#include <intr/interrupt.h>
#include <subsys/subsys.h>
#include <percpu/percpu.h>
#include <errno.h>
#include <kernel.h>
/* Raw cycles avoid clocksource_read_ns(), which needs GS before phase 7. */
extern uint64_t clocksource_cycles(void);
extern uint64_t clocksource_freq_hz(void);

/* The i8042 callback runs under the controller lock. It performs no port
 * I/O or sleep, and only takes the event lock after the i8042 lock.
 * Neither event read nor poll acquires the i8042 lock. */
#define MOUSE_RING_CAPACITY 64
static mouse_event_t events[MOUSE_RING_CAPACITY];
static unsigned event_head, event_count;
static uint64_t dropped_events;
static spinlock_T event_lock;
static list_t event_poll;

static void publish_event(const mouse_event_t *event)
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
        this_cpu()->need_resched = 1;
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
static mouse_proto_state_t packet;
static volatile bool ready;
static volatile bool command_mode;
static volatile bool stream_after_ack;
static volatile uint8_t reply[8];
static volatile unsigned reply_head, reply_tail;
static uint64_t freq_hz, probe_deadline, f5_deadline;

static bool expired(uint64_t deadline)
{
    return (int64_t)(clocksource_cycles() - deadline) >= 0;
}

static uint64_t deadline_ms(unsigned ms, uint64_t cap)
{
    uint64_t delta = freq_hz / 1000 * ms;
    uint64_t d = clocksource_cycles() + delta;
    return (int64_t)(d - cap) > 0 ? cap : d;
}

static void consume_aux(uint8_t byte)
{
    if (command_mode) {
        if (reply_tail - reply_head < sizeof(reply))
            reply[reply_tail++ % sizeof(reply)] = byte;
        if (stream_after_ack && byte == 0xFA) {
            /* F4 ACK and first packet may be drained by one pump call.
             * Switch modes in that same locked callback, before the next
             * byte is dispatched. */
            stream_after_ack = false;
            command_mode = false;
            ready = true;
        }
        return;
    }
    if (!ready)
        return;
    /* Once reporting starts, every byte is packet data. The values FA,
     * FE and AA are legal in displacement/wheel slots; only command_mode
     * interprets those values as replies. */
    mouse_event_t event;
    if (mouse_proto_feed(&packet, byte, &event) == 1)
        publish_event(&event);
}

static int wait_reply(uint8_t *out, uint64_t deadline)
{
    while (!expired(deadline)) {
        if (reply_head != reply_tail) {
            *out = reply[reply_head++ % sizeof(reply)];
            return 0;
        }
        /* i8042_pump holds its lock only for the short status/data drain.
         * Never spin with the lock held while awaiting a device reply. */
        if (i8042_pump() != I8042_OK)
            return -1;
    }
    return -1;
}

/* 0 success; -2 explicit device refusal (wheel negotiation may fall back);
 * -1 timeout/other failure. A byte has at most two RESEND retries. */
static int send_byte_with_cap(uint8_t byte, uint64_t cap)
{
    uint64_t deadline = deadline_ms(60, cap);
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        uint8_t answer;
        if (expired(deadline) || i8042_write_aux_byte(byte) != I8042_OK)
            return -1;
        if (wait_reply(&answer, deadline) != 0)
            return -1;
        if (answer == 0xFA)
            return 0;
        if (answer == 0xFC || answer == 0xFD)
            return -2;
        if (answer != 0xFE)
            return -1;
    }
    return -1;
}

static int send_byte(uint8_t byte)
{
    return send_byte_with_cap(byte, probe_deadline);
}

static int expect_byte(uint8_t expected)
{
    uint8_t answer;
    if (wait_reply(&answer, deadline_ms(60, probe_deadline)) != 0)
        return -1;
    return answer == expected ? 0 : -1;
}

static void mouse_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr; (void)parameter; (void)regs;
    (void)i8042_pump();
}

int mouse_ready(void) { return ready ? 1 : 0; }

int mouse_init(void)
{
    uint8_t original = 0;
    bool have_original = false, irq_installed = false, f4_sent = false;
    bool wheel = false;
    int rc;

    spin_init(&event_lock);
    list_init(&event_poll);
    event_head = event_count = 0;
    dropped_events = 0;
    ready = false;
    command_mode = true;
    stream_after_ack = false;
    reply_head = reply_tail = 0;
    mouse_proto_reset(&packet, false);
    if (subsys_status("keyboard") != 1)
        goto fail;
    freq_hz = clocksource_freq_hz();
    if (!freq_hz)
        goto fail;
    /* The active probe has 400 ms. F5 may run until 460 ms, reserving
     * 20 ms for a controller write begun just before that deadline and
     * another 20 ms for the rollback command-byte write. */
    uint64_t start = clocksource_cycles();
    probe_deadline = start + freq_hz * 400 / 1000;
    f5_deadline = start + freq_hz * 460 / 1000;
    if (i8042_write_controller_cmd(0xA8) != I8042_OK || expired(probe_deadline))
        goto fail;
    if (i8042_read_command_byte(&original) != I8042_OK || expired(probe_deadline))
        goto fail;
    have_original = true;
    /* Disable aux IRQ during probe even if firmware left bit 1 enabled. */
    if (i8042_write_command_byte(original & (uint8_t)~0x22) != I8042_OK)
        goto fail;
    /* read_command_byte drains pre-existing OBF bytes before its own 0x20
     * response. Only install the AUX sink after that stale traffic is gone. */
    reply_head = reply_tail = 0;
    i8042_set_aux_consumer(consume_aux);
    rc = send_byte(0xFF);
    if (rc || expect_byte(0xAA) || expect_byte(0x00))
        goto fail;
    if (send_byte(0xF6))
        goto fail;

    static const uint8_t sequence[] = { 0xF3, 200, 0xF3, 100, 0xF3, 50, 0xF2 };
    for (unsigned i = 0; i < sizeof(sequence); i++) {
        rc = send_byte(sequence[i]);
        if (rc == -2) {
            /* Explicit refusal still allows a base mouse after defaults. */
            reply_head = reply_tail = 0;
            if (send_byte(0xF6))
                goto fail;
            goto base_mouse;
        }
        if (rc)
            goto fail;
    }
    {
        uint8_t id;
        if (wait_reply(&id, deadline_ms(60, probe_deadline)))
            goto fail;
        if (id != 0x00 && id != 0x03)
            goto fail;
        wheel = id == 0x03;
    }
base_mouse:
    if (expired(probe_deadline))
        goto fail;
    if (register_irq(12, 0, mouse_handler, 0, IRQF_TRIGGER_EDGE, "mouse") != 1)
        goto fail;
    irq_installed = true;
    if (i8042_write_command_byte((original & (uint8_t)~0x20) | 0x02) != I8042_OK)
        goto fail;
    /* Drain residual AUX replies while still in command mode, then reset
     * packet alignment before enabling reporting. */
    if (i8042_pump() != I8042_OK)
        goto fail;
    reply_head = reply_tail = 0;
    mouse_proto_reset(&packet, wheel);
    f4_sent = true;
    stream_after_ack = true;
    if (send_byte(0xF4))
        goto fail;
    reply_head = reply_tail = 0;
    return 0;

fail:
    ready = false;
    stream_after_ack = false;
    if (f4_sent && freq_hz && !expired(f5_deadline)) {
        /* Best effort only: an unresponsive device cannot confirm F5. */
        reply_head = reply_tail = 0;
        command_mode = true;
        (void)send_byte_with_cap(0xF5, f5_deadline);
    }
    /* Clear bit 1 before unregister_irq, regardless of firmware's bit 1. */
    bool irq_disabled = !have_original;
    if (have_original)
        irq_disabled = i8042_write_command_byte(original & (uint8_t)~0x02) == I8042_OK;
    if (irq_installed && irq_disabled)
        (void)unregister_irq(12);
    i8042_set_aux_consumer(0);
    command_mode = false;
    reply_head = reply_tail = 0;
    mouse_proto_reset(&packet, false);
    return -1;
}

static int mouse_register(void)
{
    return register_subsys("mouse", mouse_init, SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
SUBSYS_INITCALL(mouse_register);
