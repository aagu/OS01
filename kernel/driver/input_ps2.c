/*
 * input_ps2.c — pure PS/2 Set 1 scan-sequence framer
 *
 * Turns a PS/2 Set 1 byte stream into COMPLETE scan sequences: ordinary
 * make/break, E0-prefixed keys, PrintScreen and Pause (design spec §4.3
 * / §6.1).  A complete sequence is the routing unit, so the consumer
 * never sees half a sequence.
 *
 * The framer is deliberately PURE: no locks, no device IO, no TTY, no
 * display, no physical key bitmap.  It only turns bytes into frames.
 *
 *   - the start epoch is the epoch of the FIRST byte of the sequence,
 *     cached when the sequence starts (never re-read at completion);
 *   - the two special sequences (PrintScreen, Pause) are flagged
 *     special and carry no ordinary key identity, so they can never
 *     masquerade as Shift / Ctrl;
 *   - a corrupt sequence reports EILSEQ, drops the whole partial
 *     sequence and recovers to a deterministic boundary — the bytes
 *     already consumed as part of a special sequence are never
 *     re-interpreted as standalone modifier keys.
 */

#include <driver/input_ps2.h>
#include <errno.h>

/* Set 1 prefix bytes. */
#define PS2_E0 0xE0
#define PS2_E1 0xE1

/* PrintScreen make: E0 2A E0 37 (the E0 2A is a FAKE left shift).
 * PrintScreen break: E0 B7 E0 AA (the E0 B7 is a FAKE right shift).
 * Pause: E1 1D 45 E1 9D C5 (the E1 1D 45 is a FAKE left ctrl). */
#define PS2_PS_MAKE_TAIL  0x37
#define PS2_PS_BREAK_TAIL 0xAA
#define PS2_PAUSE_CTRL    0x1D
#define PS2_PAUSE_KEY     0x45
#define PS2_PAUSE_TAIL_1  0x9D
#define PS2_PAUSE_TAIL_2  0xC5

/* Emit an ordinary key frame: the Set 1 code, or 0x100|code for an
 * E0-prefixed key. */
static void frame_ordinary(input_ps2_frame_t *out, uint8_t byte, int extended)
{
    uint8_t code = byte & 0x7F;

    out->physical_key = extended ? (uint16_t)(INPUT_PS2_KEY_E0 | code) : code;
    out->is_make     = (byte & 0x80) == 0 ? 1 : 0;
    out->is_special  = 0;
}

/* Emit a special frame (PrintScreen / Pause).  A special frame has no
 * ordinary key identity at all, so it can never be mistaken for
 * Shift / Ctrl / Alt. */
static void frame_special(input_ps2_frame_t *out, int is_make)
{
    out->physical_key = 0;
    out->is_make     = is_make ? 1 : 0;
    out->is_special  = 1;
}

/* The byte stream cannot be interpreted as a valid Set 1 sequence: drop
 * the partial sequence — the bytes already consumed as part of it are
 * never re-interpreted as standalone modifier keys — and return to a
 * deterministic boundary. */
static int frame_corrupt(input_ps2_frame_state_t *st, input_ps2_frame_t *out)
{
    st->phase   = INPUT_PS2_PH_IDLE;
    st->length  = 0;
    out->length = 0;
    return -EILSEQ;
}

void input_ps2_frame_reset(input_ps2_frame_state_t *st)
{
    st->phase  = INPUT_PS2_PH_IDLE;
    st->length = 0;
}

int input_ps2_frame_feed(input_ps2_frame_state_t *st, uint8_t byte,
                         uint64_t epoch, input_ps2_frame_t *out)
{
    if (st->phase == INPUT_PS2_PH_IDLE) {
        /* A new sequence starts here: the epoch of this FIRST byte is
         * the epoch of the sequence (never re-read at completion). */
        st->length  = 0;
        out->length = 0;
        out->start_epoch = epoch;
    }

    if (st->length >= INPUT_PS2_FRAME_MAX_BYTES)
        return frame_corrupt(st, out);

    out->bytes[st->length] = byte;
    st->length++;

    switch (st->phase) {
    case INPUT_PS2_PH_IDLE:
        if (byte == PS2_E0) {
            st->phase = INPUT_PS2_PH_E0;
            return 0;
        }
        if (byte == PS2_E1) {
            st->phase = INPUT_PS2_PH_PAUSE_1D;
            return 0;
        }
        /* An ordinary make/break is complete on its own. */
        out->length = st->length;
        frame_ordinary(out, byte, 0);
        st->phase = INPUT_PS2_PH_IDLE;
        return 1;

    case INPUT_PS2_PH_E0:
        /* E0 2A and E0 B7 open the two PrintScreen sequences.  They are
         * NOT a left/right shift press and must not update any modifier
         * state. */
        if (byte == 0x2A) {
            st->phase = INPUT_PS2_PH_PS_MAKE;
            return 0;
        }
        if (byte == 0xB7) {
            st->phase = INPUT_PS2_PH_PS_BREAK;
            return 0;
        }
        /* A prefix byte may not follow a prefix: an unrecognized E0 E0
         * (and E0 E1) is not a valid sequence. */
        if (byte == PS2_E0 || byte == PS2_E1)
            return frame_corrupt(st, out);
        /* An ordinary E0-prefixed key is complete after its code. */
        out->length = st->length;
        frame_ordinary(out, byte, 1);
        st->phase = INPUT_PS2_PH_IDLE;
        return 1;

    case INPUT_PS2_PH_PS_MAKE:
        if (byte != PS2_E0)
            return frame_corrupt(st, out);
        st->phase = INPUT_PS2_PH_PS_MAKE_E0;
        return 0;

    case INPUT_PS2_PH_PS_MAKE_E0:
        if (byte != PS2_PS_MAKE_TAIL)
            return frame_corrupt(st, out);
        out->length = st->length;
        frame_special(out, 1);
        st->phase = INPUT_PS2_PH_IDLE;
        return 1;

    case INPUT_PS2_PH_PS_BREAK:
        if (byte != PS2_E0)
            return frame_corrupt(st, out);
        st->phase = INPUT_PS2_PH_PS_BREAK_E0;
        return 0;

    case INPUT_PS2_PH_PS_BREAK_E0:
        if (byte != PS2_PS_BREAK_TAIL)
            return frame_corrupt(st, out);
        out->length = st->length;
        frame_special(out, 0);
        st->phase = INPUT_PS2_PH_IDLE;
        return 1;

    case INPUT_PS2_PH_PAUSE_1D:
        if (byte != PS2_PAUSE_CTRL)
            return frame_corrupt(st, out);
        st->phase = INPUT_PS2_PH_PAUSE_45;
        return 0;

    case INPUT_PS2_PH_PAUSE_45:
        if (byte != PS2_PAUSE_KEY)
            return frame_corrupt(st, out);
        st->phase = INPUT_PS2_PH_PAUSE_E1;
        return 0;

    case INPUT_PS2_PH_PAUSE_E1:
        if (byte != PS2_E1)
            return frame_corrupt(st, out);
        st->phase = INPUT_PS2_PH_PAUSE_9D;
        return 0;

    case INPUT_PS2_PH_PAUSE_9D:
        if (byte != PS2_PAUSE_TAIL_1)
            return frame_corrupt(st, out);
        st->phase = INPUT_PS2_PH_PAUSE_C5;
        return 0;

    case INPUT_PS2_PH_PAUSE_C5:
        if (byte != PS2_PAUSE_TAIL_2)
            return frame_corrupt(st, out);
        out->length = st->length;
        frame_special(out, 1);
        st->phase = INPUT_PS2_PH_IDLE;
        return 1;

    default:
        return frame_corrupt(st, out);
    }
}
