#ifndef _DRIVER_MOUSE_PROTO_H
#define _DRIVER_MOUSE_PROTO_H

#include <stdbool.h>
#include <stdint.h>
#include <uapi/mouse.h>

typedef struct {
    uint8_t bytes[4];
    uint8_t count;
    bool wheel_mode;
} mouse_proto_state_t;

/* -1: invalid header discarded; 0: collecting; 1: event produced. */
void mouse_proto_reset(mouse_proto_state_t *state, bool wheel_mode);
int mouse_proto_feed(mouse_proto_state_t *state, uint8_t byte, mouse_event_t *event);

#endif
