#include <driver/mouse_proto.h>

void mouse_proto_reset(mouse_proto_state_t *state, bool wheel_mode)
{
    state->count = 0;
    state->wheel_mode = wheel_mode;
}

static int16_t displacement(uint8_t low, bool negative)
{
    int16_t value = low;
    return negative ? (int16_t)(value - 256) : value;
}

int mouse_proto_feed(mouse_proto_state_t *state, uint8_t byte,
                     mouse_event_t *event)
{
    if (state->count == 0 && !(byte & 0x08))
        return -1;

    state->bytes[state->count++] = byte;
    if (state->count < (state->wheel_mode ? 4 : 3))
        return 0;

    uint8_t flags = state->bytes[0];
    int16_t x = displacement(state->bytes[1], (flags & 0x10) != 0);
    int16_t y = displacement(state->bytes[2], (flags & 0x20) != 0);
    int8_t wheel = 0;
    if (state->wheel_mode) {
        uint8_t nibble = state->bytes[3] & 0x0f;
        wheel = (int8_t)((nibble & 0x08) ? (int)nibble - 16 : nibble);
    }
    event->buttons = flags & 0x07;
    event->wheel = wheel;
    event->dx = (flags & 0x40) ? 0 : x;
    event->dy = (flags & 0x80) ? 0 : (int16_t)-y;
    event->reserved = 0;
    state->count = 0;
    return 1;
}
