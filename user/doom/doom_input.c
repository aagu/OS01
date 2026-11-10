#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/fb.h>
#include <gfx.h>
#include <gui/gui_input_ps2.h>
#include "doom_input.h"
#include "doomkeys.h"

#ifndef KEY_FIRE
#define KEY_FIRE       0xa3
#endif
#ifndef KEY_USE
#define KEY_USE        0xa2
#endif
#ifndef KEY_RSHIFT
#define KEY_RSHIFT     (0x80+0x36)
#endif
#ifndef KEY_LALT
#define KEY_LALT       (0x80+0x38)
#endif
#ifndef KEY_UPARROW
#define KEY_UPARROW    0xad
#endif
#ifndef KEY_DOWNARROW
#define KEY_DOWNARROW  0xaf
#endif
#ifndef KEY_LEFTARROW
#define KEY_LEFTARROW  0xac
#endif
#ifndef KEY_RIGHTARROW
#define KEY_RIGHTARROW 0xae
#endif
#ifndef KEY_ENTER
#define KEY_ENTER      13
#endif
#ifndef KEY_ESCAPE
#define KEY_ESCAPE     27
#endif
#ifndef KEY_TAB
#define KEY_TAB        9
#endif
#ifndef KEY_BACKSPACE
#define KEY_BACKSPACE  127
#endif

static unsigned char map_ps2_to_doom(uint16_t physical_key)
{
    /* Arrow keys (E0 extended) */
    if (physical_key == 0x148) return KEY_UPARROW;
    if (physical_key == 0x14B) return KEY_LEFTARROW;
    if (physical_key == 0x14D) return KEY_RIGHTARROW;
    if (physical_key == 0x150) return KEY_DOWNARROW;

    /* Extended Enter */
    if (physical_key == 0x11C) return KEY_ENTER;

    /* Extended RCtrl and RAlt */
    if (physical_key == 0x11D) return KEY_FIRE;
    if (physical_key == 0x138) return KEY_LALT;

    /* Bare non-extended keys */
    if (!(physical_key & GUI_PS2_KEY_E0)) {
        uint8_t sc = (uint8_t)physical_key;
        switch (sc) {
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_FIRE;  /* LCtrl */
        case 0x38: return KEY_LALT;  /* LAlt */
        case 0x2A: return KEY_RSHIFT;/* LShift -> KEY_RSHIFT */
        case 0x36: return KEY_RSHIFT;/* RShift -> KEY_RSHIFT */
        case 0x39: return KEY_USE;   /* Space */

        /* Digits */
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x0C: return '-';
        case 0x0D: return '=';

        /* Letters (always lowercase) */
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1A: return '[';
        case 0x1B: return ']';

        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x27: return ';';
        case 0x28: return '\'';
        case 0x29: return '`';

        case 0x2B: return '\\';
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x33: return ',';
        case 0x34: return '.';
        case 0x35: return '/';

        /* Function keys F1..F10 */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        default: return 0;
        }
    }
    return 0;
}

static int check_owner(doom_input_t *in)
{
    if (!in || !in->gfx_handle) return -1;
    gfx_display_state_t st;
    if (gfx_get_display_state(in->gfx_handle, &st) < 0) {
        return -1;
    }
    if (!(st.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int queue_push(doom_input_t *in, int pressed, unsigned char key)
{
    if (in->count >= DOOM_INPUT_QUEUE_CAP) {
        in->head = in->tail = in->count = 0;
        in->error = 1;
        errno = ENOBUFS;
        return -1;
    }
    in->queue[in->tail].pressed = pressed;
    in->queue[in->tail].key = key;
    in->tail = (in->tail + 1) % DOOM_INPUT_QUEUE_CAP;
    in->count++;
    return 0;
}

int doom_input_open(doom_input_t *in, gfx_handle_t *h)
{
    if (!in || !h) {
        errno = EINVAL;
        return -1;
    }
    memset(in, 0, sizeof(*in));
    in->kbd_fd = -1;

    int fd = open("/dev/keyboard", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    gfx_input_state_t st;
    if (gfx_bind_input(h, fd, -1, &st) < 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }

    in->kbd_fd = fd;
    in->gfx_handle = h;
    in->bound = 1;

    uint16_t initial_mods = (st.initial_modifiers & 1) ? GUI_MOD_CAPS : 0;
    gui_ps2_init(&in->ps2, initial_mods);

    return 0;
}

int doom_input_pump(doom_input_t *in, uint64_t now_ms)
{
    if (!in) {
        errno = EINVAL;
        return -1;
    }
    if (in->error) {
        in->head = in->tail = in->count = 0;
        return -1;
    }
    if (check_owner(in) < 0) {
        in->error = 1;
        in->head = in->tail = in->count = 0;
        return -1;
    }

    uint8_t buf[256];
    ssize_t n = read(in->kbd_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            gui_ps2_expire(&in->ps2, now_ms);
            return 0;
        }
        in->error = 1;
        in->head = in->tail = in->count = 0;
        return -1;
    }
    if (n == 0) {
        in->error = 1;
        in->head = in->tail = in->count = 0;
        errno = EIO;
        return -1;
    }

    for (ssize_t i = 0; i < n; i++) {
        gui_key_event_t ev;
        int rc = gui_ps2_feed(&in->ps2, buf[i], now_ms, &ev);
        if (rc == 1) {
            if (ev.action == GUI_KEY_REPEAT) {
                continue;
            }
            unsigned char dkey = map_ps2_to_doom(ev.physical_key);
            if (dkey == 0) {
                continue;
            }
            unsigned slot = gui_ps2_key_slot(ev.physical_key);
            if (ev.action == GUI_KEY_DOWN) {
                int was_down = (in->phys_down[slot / 8] >> (slot % 8)) & 1;
                if (was_down) {
                    continue;
                }
                in->phys_down[slot / 8] |= (uint8_t)(1u << (slot % 8));
                uint8_t ref = in->logical_refcount[dkey];
                in->logical_refcount[dkey] = ref + 1;
                if (ref == 0) {
                    if (queue_push(in, 1, dkey) < 0) {
                        return -1;
                    }
                }
            } else if (ev.action == GUI_KEY_UP) {
                int was_down = (in->phys_down[slot / 8] >> (slot % 8)) & 1;
                if (!was_down) {
                    continue;
                }
                in->phys_down[slot / 8] &= (uint8_t)~(1u << (slot % 8));
                if (in->logical_refcount[dkey] > 0) {
                    in->logical_refcount[dkey]--;
                    if (in->logical_refcount[dkey] == 0) {
                        if (queue_push(in, 0, dkey) < 0) {
                            return -1;
                        }
                    }
                }
            }
        } else if (rc < 0) {
            in->error = 1;
            in->head = in->tail = in->count = 0;
            return -1;
        }
    }

    gui_ps2_expire(&in->ps2, now_ms);
    return 0;
}

int doom_input_pop(doom_input_t *in, int *pressed, unsigned char *key)
{
    if (!in || !pressed || !key) {
        errno = EINVAL;
        return -1;
    }
    if (in->error) {
        in->head = in->tail = in->count = 0;
        return -1;
    }
    if (check_owner(in) < 0) {
        in->error = 1;
        in->head = in->tail = in->count = 0;
        return -1;
    }
    if (in->count == 0) {
        return 0;
    }
    *pressed = in->queue[in->head].pressed;
    *key = in->queue[in->head].key;
    in->head = (in->head + 1) % DOOM_INPUT_QUEUE_CAP;
    in->count--;
    return 1;
}

void doom_input_close(doom_input_t *in)
{
    if (!in) return;
    in->head = in->tail = in->count = 0;
    memset(in->logical_refcount, 0, sizeof(in->logical_refcount));
    memset(in->phys_down, 0, sizeof(in->phys_down));
    if (in->bound && in->gfx_handle) {
        gfx_unbind_input(in->gfx_handle);
        in->bound = 0;
    }
    if (in->kbd_fd >= 0) {
        close(in->kbd_fd);
        in->kbd_fd = -1;
    }
    in->gfx_handle = NULL;
}
