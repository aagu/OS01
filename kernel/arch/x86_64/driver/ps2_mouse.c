/* kernel/driver/ps2_mouse.c — x86 PS/2 (i8042 aux) mouse producer */
#include <stdbool.h>
#include <stdint.h>
#include <driver/i8042.h>
#include <driver/mouse.h>
#include <driver/mouse_proto.h>
#include <driver/input_route.h>
#include <intr/interrupt.h>
#include <arch/irq.h>
#include <subsys/subsys.h>

/* Raw cycles avoid clocksource_read_ns(), which needs GS before phase 7. */
extern uint64_t clocksource_cycles(void);
extern uint64_t clocksource_freq_hz(void);

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
            stream_after_ack = false;
            command_mode = false;
            ready = true;
        }
        return;
    }
    if (!ready)
        return;
    mouse_event_t event;
    if (mouse_proto_feed(&packet, byte, &event) == 1)
        input_route_mouse_event(&event);
}

static int wait_reply(uint8_t *out, uint64_t deadline)
{
    while (!expired(deadline)) {
        if (reply_head != reply_tail) {
            *out = reply[reply_head++ % sizeof(reply)];
            return 0;
        }
        if (i8042_pump() != I8042_OK)
            return -1;
    }
    return -1;
}

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
    uint32_t mouse_gsi = arch_irq_isa_to_gsi(12);
    bool wheel = false;
    int rc;

    mouse_input_init();
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

    uint64_t start = clocksource_cycles();
    probe_deadline = start + freq_hz * 400 / 1000;
    f5_deadline = start + freq_hz * 460 / 1000;
    if (i8042_write_controller_cmd(0xA8) != I8042_OK || expired(probe_deadline))
        goto fail;
    if (i8042_read_command_byte(&original) != I8042_OK || expired(probe_deadline))
        goto fail;
    have_original = true;
    if (i8042_write_command_byte(original & (uint8_t)~0x22) != I8042_OK)
        goto fail;
    reply_head = reply_tail = 0;
    i8042_set_aux_consumer(consume_aux);
    rc = send_byte(0xFF);
    if (rc || expect_byte(0xAA) || expect_byte(0x00))
        goto fail;
    if (send_byte(0xF6))
        goto fail;

    static const uint8_t sequence[] = { 0xF3, 200, 0xF3, 100, 0xF3, 80, 0xF2 };
    for (unsigned i = 0; i < sizeof(sequence); i++) {
        rc = send_byte(sequence[i]);
        if (rc == -2) {
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
    if (register_irq(mouse_gsi, 0, mouse_handler, 0, IRQF_TRIGGER_EDGE, "mouse") != 1)
        goto fail;
    irq_installed = true;
    if (i8042_write_command_byte((original & (uint8_t)~0x20) | 0x02) != I8042_OK)
        goto fail;
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
        reply_head = reply_tail = 0;
        command_mode = true;
        (void)send_byte_with_cap(0xF5, f5_deadline);
    }
    bool irq_disabled = !have_original;
    if (have_original)
        irq_disabled = i8042_write_command_byte(original & (uint8_t)~0x02) == I8042_OK;
    if (irq_installed && irq_disabled)
        (void)unregister_irq(mouse_gsi);
    i8042_set_aux_consumer(0);
    command_mode = false;
    reply_head = reply_tail = 0;
    mouse_proto_reset(&packet, false);
    return -1;
}
