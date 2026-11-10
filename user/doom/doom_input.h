#ifndef _DOOM_INPUT_H
#define _DOOM_INPUT_H

#include <stdint.h>
#include <gfx.h>
#include <gui/gui_input_ps2.h>

#define DOOM_INPUT_QUEUE_CAP 256

typedef struct {
    int pressed;
    unsigned char key;
} doom_input_event_t;

typedef struct doom_input {
    int kbd_fd;
    gfx_handle_t *gfx_handle;
    int bound;
    int error;
    gui_ps2_state_t ps2;
    uint8_t phys_down[GUI_PS2_KEY_SLOTS / 8];
    uint8_t logical_refcount[256];
    doom_input_event_t queue[DOOM_INPUT_QUEUE_CAP];
    int head;
    int tail;
    int count;
} doom_input_t;

int doom_input_open(doom_input_t *in, gfx_handle_t *h);
int doom_input_pump(doom_input_t *in, uint64_t now_ms);
int doom_input_pop(doom_input_t *in, int *pressed, unsigned char *key);
void doom_input_close(doom_input_t *in);

#endif /* _DOOM_INPUT_H */
