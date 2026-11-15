#include <driver/usb_hid_report.h>
#include <errno.h>
#include <string.h>

/* ── HID to PS/2 Set 1 mapping ───────────────────────────────── */

enum {
    HID_KEY_UNMAPPED = 0,
    HID_KEY_ORDINARY,
    HID_KEY_PRINTSCREEN,
    HID_KEY_PAUSE,
};

struct hid_mapping {
    uint8_t type;     /* HID_KEY_* */
    uint8_t scancode; /* 7-bit Set 1 scancode */
    uint8_t extended; /* 1 if E0-prefixed */
};

static const struct hid_mapping s_hid_map[0x68] = {
    /* 0x00..0x03: reserved/errors -> unmapped */
    [0x04] = { HID_KEY_ORDINARY, 0x1E, 0 }, /* a */
    [0x05] = { HID_KEY_ORDINARY, 0x30, 0 }, /* b */
    [0x06] = { HID_KEY_ORDINARY, 0x2E, 0 }, /* c */
    [0x07] = { HID_KEY_ORDINARY, 0x20, 0 }, /* d */
    [0x08] = { HID_KEY_ORDINARY, 0x12, 0 }, /* e */
    [0x09] = { HID_KEY_ORDINARY, 0x21, 0 }, /* f */
    [0x0A] = { HID_KEY_ORDINARY, 0x22, 0 }, /* g */
    [0x0B] = { HID_KEY_ORDINARY, 0x23, 0 }, /* h */
    [0x0C] = { HID_KEY_ORDINARY, 0x17, 0 }, /* i */
    [0x0D] = { HID_KEY_ORDINARY, 0x24, 0 }, /* j */
    [0x0E] = { HID_KEY_ORDINARY, 0x25, 0 }, /* k */
    [0x0F] = { HID_KEY_ORDINARY, 0x26, 0 }, /* l */
    [0x10] = { HID_KEY_ORDINARY, 0x32, 0 }, /* m */
    [0x11] = { HID_KEY_ORDINARY, 0x31, 0 }, /* n */
    [0x12] = { HID_KEY_ORDINARY, 0x18, 0 }, /* o */
    [0x13] = { HID_KEY_ORDINARY, 0x19, 0 }, /* p */
    [0x14] = { HID_KEY_ORDINARY, 0x10, 0 }, /* q */
    [0x15] = { HID_KEY_ORDINARY, 0x13, 0 }, /* r */
    [0x16] = { HID_KEY_ORDINARY, 0x1F, 0 }, /* s */
    [0x17] = { HID_KEY_ORDINARY, 0x14, 0 }, /* t */
    [0x18] = { HID_KEY_ORDINARY, 0x16, 0 }, /* u */
    [0x19] = { HID_KEY_ORDINARY, 0x2F, 0 }, /* v */
    [0x1A] = { HID_KEY_ORDINARY, 0x11, 0 }, /* w */
    [0x1B] = { HID_KEY_ORDINARY, 0x2D, 0 }, /* x */
    [0x1C] = { HID_KEY_ORDINARY, 0x15, 0 }, /* y */
    [0x1D] = { HID_KEY_ORDINARY, 0x2C, 0 }, /* z */

    /* Digits 1-9, 0 */
    [0x1E] = { HID_KEY_ORDINARY, 0x02, 0 }, /* 1 */
    [0x1F] = { HID_KEY_ORDINARY, 0x03, 0 }, /* 2 */
    [0x20] = { HID_KEY_ORDINARY, 0x04, 0 }, /* 3 */
    [0x21] = { HID_KEY_ORDINARY, 0x05, 0 }, /* 4 */
    [0x22] = { HID_KEY_ORDINARY, 0x06, 0 }, /* 5 */
    [0x23] = { HID_KEY_ORDINARY, 0x07, 0 }, /* 6 */
    [0x24] = { HID_KEY_ORDINARY, 0x08, 0 }, /* 7 */
    [0x25] = { HID_KEY_ORDINARY, 0x09, 0 }, /* 8 */
    [0x26] = { HID_KEY_ORDINARY, 0x0A, 0 }, /* 9 */
    [0x27] = { HID_KEY_ORDINARY, 0x0B, 0 }, /* 0 */

    /* Control / punctuation */
    [0x28] = { HID_KEY_ORDINARY, 0x1C, 0 }, /* Enter */
    [0x29] = { HID_KEY_ORDINARY, 0x01, 0 }, /* Escape */
    [0x2A] = { HID_KEY_ORDINARY, 0x0E, 0 }, /* Backspace */
    [0x2B] = { HID_KEY_ORDINARY, 0x0F, 0 }, /* Tab */
    [0x2C] = { HID_KEY_ORDINARY, 0x39, 0 }, /* Space */
    [0x2D] = { HID_KEY_ORDINARY, 0x0C, 0 }, /* - */
    [0x2E] = { HID_KEY_ORDINARY, 0x0D, 0 }, /* = */
    [0x2F] = { HID_KEY_ORDINARY, 0x1A, 0 }, /* [ */
    [0x30] = { HID_KEY_ORDINARY, 0x1B, 0 }, /* ] */
    [0x31] = { HID_KEY_ORDINARY, 0x2B, 0 }, /* \ */
    [0x32] = { HID_KEY_ORDINARY, 0x2B, 0 }, /* Non-US #/~ */
    [0x33] = { HID_KEY_ORDINARY, 0x27, 0 }, /* ; */
    [0x34] = { HID_KEY_ORDINARY, 0x28, 0 }, /* ' */
    [0x35] = { HID_KEY_ORDINARY, 0x29, 0 }, /* ` */
    [0x36] = { HID_KEY_ORDINARY, 0x33, 0 }, /* , */
    [0x37] = { HID_KEY_ORDINARY, 0x34, 0 }, /* . */
    [0x38] = { HID_KEY_ORDINARY, 0x35, 0 }, /* / */
    [0x39] = { HID_KEY_ORDINARY, 0x3A, 0 }, /* Caps Lock */

    /* Function keys F1–F12 */
    [0x3A] = { HID_KEY_ORDINARY, 0x3B, 0 }, /* F1 */
    [0x3B] = { HID_KEY_ORDINARY, 0x3C, 0 }, /* F2 */
    [0x3C] = { HID_KEY_ORDINARY, 0x3D, 0 }, /* F3 */
    [0x3D] = { HID_KEY_ORDINARY, 0x3E, 0 }, /* F4 */
    [0x3E] = { HID_KEY_ORDINARY, 0x3F, 0 }, /* F5 */
    [0x3F] = { HID_KEY_ORDINARY, 0x40, 0 }, /* F6 */
    [0x40] = { HID_KEY_ORDINARY, 0x41, 0 }, /* F7 */
    [0x41] = { HID_KEY_ORDINARY, 0x42, 0 }, /* F8 */
    [0x42] = { HID_KEY_ORDINARY, 0x43, 0 }, /* F9 */
    [0x43] = { HID_KEY_ORDINARY, 0x44, 0 }, /* F10 */
    [0x44] = { HID_KEY_ORDINARY, 0x57, 0 }, /* F11 */
    [0x45] = { HID_KEY_ORDINARY, 0x58, 0 }, /* F12 */

    /* Special & Navigation */
    [0x46] = { HID_KEY_PRINTSCREEN, 0, 0 }, /* PrintScreen */
    [0x47] = { HID_KEY_ORDINARY, 0x46, 0 }, /* Scroll Lock */
    [0x48] = { HID_KEY_PAUSE, 0, 0 },       /* Pause */
    [0x49] = { HID_KEY_ORDINARY, 0x52, 1 }, /* Insert -> E0 52 */
    [0x4A] = { HID_KEY_ORDINARY, 0x47, 1 }, /* Home -> E0 47 */
    [0x4B] = { HID_KEY_ORDINARY, 0x49, 1 }, /* Page Up -> E0 49 */
    [0x4C] = { HID_KEY_ORDINARY, 0x53, 1 }, /* Delete -> E0 53 */
    [0x4D] = { HID_KEY_ORDINARY, 0x4F, 1 }, /* End -> E0 4F */
    [0x4E] = { HID_KEY_ORDINARY, 0x51, 1 }, /* Page Down -> E0 51 */
    [0x4F] = { HID_KEY_ORDINARY, 0x4D, 1 }, /* Right Arrow -> E0 4D */
    [0x50] = { HID_KEY_ORDINARY, 0x4B, 1 }, /* Left Arrow -> E0 4B */
    [0x51] = { HID_KEY_ORDINARY, 0x50, 1 }, /* Down Arrow -> E0 50 */
    [0x52] = { HID_KEY_ORDINARY, 0x48, 1 }, /* Up Arrow -> E0 48 */

    /* Numeric Keypad */
    [0x53] = { HID_KEY_ORDINARY, 0x45, 0 }, /* Num Lock */
    [0x54] = { HID_KEY_ORDINARY, 0x35, 1 }, /* KP / -> E0 35 */
    [0x55] = { HID_KEY_ORDINARY, 0x37, 0 }, /* KP * */
    [0x56] = { HID_KEY_ORDINARY, 0x4A, 0 }, /* KP - */
    [0x57] = { HID_KEY_ORDINARY, 0x4E, 0 }, /* KP + */
    [0x58] = { HID_KEY_ORDINARY, 0x1C, 1 }, /* KP Enter -> E0 1C */
    [0x59] = { HID_KEY_ORDINARY, 0x4F, 0 }, /* KP 1 */
    [0x5A] = { HID_KEY_ORDINARY, 0x50, 0 }, /* KP 2 */
    [0x5B] = { HID_KEY_ORDINARY, 0x51, 0 }, /* KP 3 */
    [0x5C] = { HID_KEY_ORDINARY, 0x4B, 0 }, /* KP 4 */
    [0x5D] = { HID_KEY_ORDINARY, 0x4C, 0 }, /* KP 5 */
    [0x5E] = { HID_KEY_ORDINARY, 0x4D, 0 }, /* KP 6 */
    [0x5F] = { HID_KEY_ORDINARY, 0x47, 0 }, /* KP 7 */
    [0x60] = { HID_KEY_ORDINARY, 0x48, 0 }, /* KP 8 */
    [0x61] = { HID_KEY_ORDINARY, 0x49, 0 }, /* KP 9 */
    [0x62] = { HID_KEY_ORDINARY, 0x52, 0 }, /* KP 0 */
    [0x63] = { HID_KEY_ORDINARY, 0x53, 0 }, /* KP . */
    [0x64] = { HID_KEY_ORDINARY, 0x56, 0 }, /* Non-US \ */
    [0x65] = { HID_KEY_ORDINARY, 0x5D, 1 }, /* Application / Menu -> E0 5D */
};

struct hid_mod_mapping {
    uint8_t scancode;
    uint8_t extended;
};

static const struct hid_mod_mapping s_hid_mods[8] = {
    [0] = { 0x1D, 0 }, /* Left Ctrl: 1D */
    [1] = { 0x2A, 0 }, /* Left Shift: 2A */
    [2] = { 0x38, 0 }, /* Left Alt: 38 */
    [3] = { 0x5B, 1 }, /* Left GUI: E0 5B */
    [4] = { 0x1D, 1 }, /* Right Ctrl: E0 1D */
    [5] = { 0x36, 0 }, /* Right Shift: 36 */
    [6] = { 0x38, 1 }, /* Right Alt: E0 38 */
    [7] = { 0x5C, 1 }, /* Right GUI: E0 5C */
};

/* ── Frame emission helpers ──────────────────────────────────── */

static void emit_mod_frame(input_ps2_frame_t *frame, unsigned bit, int is_make)
{
    const struct hid_mod_mapping *m = &s_hid_mods[bit];
    memset(frame, 0, sizeof(*frame));
    frame->is_make = is_make ? 1 : 0;
    frame->is_special = 0;
    if (m->extended) {
        frame->length = 2;
        frame->bytes[0] = 0xE0;
        frame->bytes[1] = is_make ? m->scancode : (uint8_t)(m->scancode | 0x80);
        frame->physical_key = (uint16_t)(INPUT_PS2_KEY_E0 | m->scancode);
    } else {
        frame->length = 1;
        frame->bytes[0] = is_make ? m->scancode : (uint8_t)(m->scancode | 0x80);
        frame->physical_key = m->scancode;
    }
}

static int emit_key_frame(input_ps2_frame_t *frame, uint8_t usage, int is_make)
{
    if (usage >= sizeof(s_hid_map) / sizeof(s_hid_map[0]))
        return 0;

    const struct hid_mapping *m = &s_hid_map[usage];
    if (m->type == HID_KEY_UNMAPPED)
        return 0;

    memset(frame, 0, sizeof(*frame));

    if (m->type == HID_KEY_PRINTSCREEN) {
        frame->length = 4;
        frame->bytes[0] = 0xE0;
        frame->bytes[1] = is_make ? 0x2A : 0xB7;
        frame->bytes[2] = 0xE0;
        frame->bytes[3] = is_make ? 0x37 : 0xAA;
        frame->physical_key = 0; /* PrintScreen physical identity is route-private */
        frame->is_make = is_make ? 1 : 0;
        frame->is_special = 1;
        return 1;
    }

    if (m->type == HID_KEY_PAUSE) {
        if (!is_make) {
            /* Pause has no break sequence in PS/2 Set 1 */
            return 0;
        }
        frame->length = 6;
        frame->bytes[0] = 0xE1;
        frame->bytes[1] = 0x1D;
        frame->bytes[2] = 0x45;
        frame->bytes[3] = 0xE1;
        frame->bytes[4] = 0x9D;
        frame->bytes[5] = 0xC5;
        frame->physical_key = 0;
        frame->is_make = 1;
        frame->is_special = 1;
        return 1;
    }

    /* Ordinary key */
    frame->is_make = is_make ? 1 : 0;
    frame->is_special = 0;
    if (m->extended) {
        frame->length = 2;
        frame->bytes[0] = 0xE0;
        frame->bytes[1] = is_make ? m->scancode : (uint8_t)(m->scancode | 0x80);
        frame->physical_key = (uint16_t)(INPUT_PS2_KEY_E0 | m->scancode);
    } else {
        frame->length = 1;
        frame->bytes[0] = is_make ? m->scancode : (uint8_t)(m->scancode | 0x80);
        frame->physical_key = m->scancode;
    }
    return 1;
}

/* ── Keyboard Boot Report Decoder ────────────────────────────── */

int usb_hid_keyboard_update(usb_hid_keyboard_t *state, const uint8_t *report,
                            size_t len, input_ps2_frame_t out[USB_HID_MAX_FRAMES])
{
    if (state == NULL || report == NULL || out == NULL || len != 8)
        return -EINVAL;

    uint8_t new_modifiers = report[0];
    int frame_count = 0;

    /* Check for ErrorRollOver / ErrorUndefined (0x01..0x03) in the key array */
    int has_rollover = 0;
    for (size_t i = 2; i < 8; i++) {
        if (report[i] >= 0x01 && report[i] <= 0x03) {
            has_rollover = 1;
            break;
        }
    }

    if (has_rollover) {
        /* Rollover: preserve previous non-modifier key set, only update modifiers */
        uint8_t prev_modifiers = state->modifiers;

        /* Group 2: Modifier breaks (ascending bit 0..7) */
        uint8_t released_mods = prev_modifiers & ~new_modifiers;
        for (unsigned bit = 0; bit < 8; bit++) {
            if (released_mods & (1u << bit)) {
                emit_mod_frame(&out[frame_count++], bit, 0);
            }
        }

        /* Group 3: Modifier makes (ascending bit 0..7) */
        uint8_t pressed_mods = new_modifiers & ~prev_modifiers;
        for (unsigned bit = 0; bit < 8; bit++) {
            if (pressed_mods & (1u << bit)) {
                emit_mod_frame(&out[frame_count++], bit, 1);
            }
        }

        state->modifiers = new_modifiers;
        return frame_count;
    }

    /* Normal report: collect unique, mapped usages from report[2..7] */
    uint8_t new_keys[6];
    uint8_t new_key_count = 0;

    for (size_t i = 2; i < 8; i++) {
        uint8_t u = report[i];
        if (u == 0)
            continue;
        if (u >= sizeof(s_hid_map) / sizeof(s_hid_map[0]) ||
            s_hid_map[u].type == HID_KEY_UNMAPPED)
            continue;

        /* Check for duplicate in new_keys */
        int duplicate = 0;
        for (uint8_t k = 0; k < new_key_count; k++) {
            if (new_keys[k] == u) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate && new_key_count < 6) {
            new_keys[new_key_count++] = u;
        }
    }

    /* Sort new_keys ascending by HID usage */
    for (uint8_t i = 1; i < new_key_count; i++) {
        uint8_t key = new_keys[i];
        int j = (int)i - 1;
        while (j >= 0 && new_keys[j] > key) {
            new_keys[j + 1] = new_keys[j];
            j--;
        }
        new_keys[j + 1] = key;
    }

    uint8_t prev_modifiers = state->modifiers;
    const uint8_t *prev_keys = state->keys;
    uint8_t prev_key_count = state->key_count;

    /* Spec four-group emission order:
     * 1. Ordinary key break (ascending by usage)
     * 2. Modifier break (ascending by usage/bit)
     * 3. Modifier make (ascending by usage/bit)
     * 4. Ordinary key make (ascending by usage)
     */

    /* Group 1: Ordinary key break (present in prev_keys, not in new_keys) */
    for (uint8_t i = 0; i < prev_key_count; i++) {
        uint8_t pu = prev_keys[i];
        if (pu == 0)
            continue;
        int still_pressed = 0;
        for (uint8_t j = 0; j < new_key_count; j++) {
            if (new_keys[j] == pu) {
                still_pressed = 1;
                break;
            }
        }
        if (!still_pressed) {
            if (emit_key_frame(&out[frame_count], pu, 0))
                frame_count++;
        }
    }

    /* Group 2: Modifier break */
    uint8_t released_mods = prev_modifiers & ~new_modifiers;
    for (unsigned bit = 0; bit < 8; bit++) {
        if (released_mods & (1u << bit)) {
            emit_mod_frame(&out[frame_count++], bit, 0);
        }
    }

    /* Group 3: Modifier make */
    uint8_t pressed_mods = new_modifiers & ~prev_modifiers;
    for (unsigned bit = 0; bit < 8; bit++) {
        if (pressed_mods & (1u << bit)) {
            emit_mod_frame(&out[frame_count++], bit, 1);
        }
    }

    /* Group 4: Ordinary key make (present in new_keys, not in prev_keys) */
    for (uint8_t i = 0; i < new_key_count; i++) {
        uint8_t nu = new_keys[i];
        int was_pressed = 0;
        for (uint8_t j = 0; j < prev_key_count; j++) {
            if (prev_keys[j] == nu) {
                was_pressed = 1;
                break;
            }
        }
        if (!was_pressed) {
            if (emit_key_frame(&out[frame_count], nu, 1))
                frame_count++;
        }
    }

    /* Commit entire state atomically after calculation */
    state->modifiers = new_modifiers;
    state->key_count = new_key_count;
    for (uint8_t i = 0; i < 6; i++) {
        state->keys[i] = (i < new_key_count) ? new_keys[i] : 0;
    }

    return frame_count;
}

size_t usb_hid_keyboard_release(usb_hid_keyboard_t *state,
                               input_ps2_frame_t out[USB_HID_MAX_FRAMES])
{
    if (state == NULL || out == NULL)
        return 0;

    size_t frame_count = 0;

    /* 1. Ordinary key break for all held keys in ascending order of usage */
    for (uint8_t i = 0; i < state->key_count; i++) {
        uint8_t u = state->keys[i];
        if (u != 0) {
            if (emit_key_frame(&out[frame_count], u, 0))
                frame_count++;
        }
    }

    /* 2. Modifier break for all held modifiers in ascending bit order */
    for (unsigned bit = 0; bit < 8; bit++) {
        if (state->modifiers & (1u << bit)) {
            emit_mod_frame(&out[frame_count++], bit, 0);
        }
    }

    /* 3. Clear state */
    memset(state, 0, sizeof(*state));

    return frame_count;
}

/* ── Mouse Boot Report Decoder ───────────────────────────────── */

int usb_hid_mouse_decode(const uint8_t *report, size_t len, uint16_t mps,
                         mouse_event_t *out)
{
    if (report == NULL || out == NULL)
        return -EINVAL;

    if (mps < 3 || len < 3 || len > mps)
        return -EINVAL;

    out->buttons = report[0] & 0x07;
    out->wheel = 0;
    out->dx = (int16_t)(int8_t)report[1];
    out->dy = (int16_t)(int8_t)report[2];
    out->reserved = 0;

    return 0;
}
