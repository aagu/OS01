#ifndef __INPUT_PS2_H__
#define __INPUT_PS2_H__

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════
 *  PS/2 Set 1 scan-sequence framer (pure state machine)
 *
 *  Turns a PS/2 Set 1 byte stream into COMPLETE scan sequences:
 *  ordinary make/break, E0-prefixed keys, PrintScreen and Pause.
 *  A complete sequence is the routing unit (design spec §4.3): the
 *  consumer never sees half a sequence, so a half-emitted "E0 1D"
 *  can never be mistaken for a bare left Ctrl and a swallowed "E0"
 *  can never turn the following "1D" into a phantom left Ctrl.
 *
 *  The framer is deliberately PURE: no locks, no device IO, no TTY,
 *  no display, no physical key bitmap.  It only turns bytes into
 *  frames.  The routing layer (input_route.c) consumes it.
 * ═══════════════════════════════════════════════════════════════ */

/* Longest sequence the framer recognises: the Pause key sends
 * E1 1D 45 E1 9D C5 (6 bytes); PrintScreen uses 4 bytes. */
#define INPUT_PS2_FRAME_MAX_BYTES 6

/* Physical key identity of an E0-prefixed key: 0x100 | Set 1 code
 * (0x11D = right Ctrl).  Ordinary keys use the bare Set 1 code
 * (0x1E = A). */
#define INPUT_PS2_KEY_E0 0x100u

/* Parser phase — the whole persistent state of the framer.  Each phase
 * knows exactly which byte is expected next. */
enum {
    INPUT_PS2_PH_IDLE = 0,     /* no partial sequence (sequence boundary) */
    INPUT_PS2_PH_E0,           /* consumed E0: expecting one key code */
    INPUT_PS2_PH_PS_MAKE,      /* consumed E0 2A: expecting E0 */
    INPUT_PS2_PH_PS_MAKE_E0,   /* consumed E0 2A E0: expecting 37 */
    INPUT_PS2_PH_PS_BREAK,     /* consumed E0 B7: expecting E0 */
    INPUT_PS2_PH_PS_BREAK_E0,  /* consumed E0 B7 E0: expecting AA */
    INPUT_PS2_PH_PAUSE_1D,     /* consumed E1: expecting 1D */
    INPUT_PS2_PH_PAUSE_45,     /* consumed E1 1D: expecting 45 */
    INPUT_PS2_PH_PAUSE_E1,     /* consumed E1 1D 45: expecting E1 */
    INPUT_PS2_PH_PAUSE_9D,     /* consumed E1 1D 45 E1: expecting 9D */
    INPUT_PS2_PH_PAUSE_C5,     /* consumed E1 1D 45 E1 9D: expecting C5 */
};

/* Framer state — stack allocatable and tiny: the accumulated bytes and
 * the start epoch live in the caller's frame buffer, so the persistent
 * state stays at 2 bytes (the brief caps it at 6). */
typedef struct {
    uint8_t phase;   /* INPUT_PS2_PH_* */
    uint8_t length;  /* bytes of the current sequence consumed so far */
} input_ps2_frame_state_t;

/* One complete scan sequence. */
typedef struct {
    uint8_t  bytes[INPUT_PS2_FRAME_MAX_BYTES]; /* raw Set 1 bytes */
    uint8_t  length;      /* number of valid bytes */
    uint16_t physical_key;/* Set 1 code, or 0x100|code for E0 keys */
    uint8_t  is_make;     /* 1 = key down (make), 0 = key up (break) */
    uint8_t  is_special;  /* 1 = PrintScreen/Pause (never a modifier) */
    uint64_t start_epoch; /* epoch of the FIRST byte of the sequence */
} input_ps2_frame_t;

/* A special frame carries no ordinary key identity (physical_key is 0),
 * so it can never be mistaken for Shift / Ctrl / Alt.  Its is_make flag
 * marks the PrintScreen press (1) / release (0); the Pause key has no
 * separate release and is reported as a press (1). */

_Static_assert(sizeof(input_ps2_frame_state_t) <= 6,
               "input_ps2_frame_state_t must stay within 6 bytes");

/* Drop a partial sequence and return to the "no partial sequence"
 * boundary.  The next byte starts a fresh sequence.  A zeroed state is
 * already at that boundary, so a static framer state needs no init. */
void input_ps2_frame_reset(input_ps2_frame_state_t *st);

/* Feed one Set 1 byte into the framer.
 *
 * The SAME `out` buffer must be passed for every byte of one sequence:
 * it is the sequence accumulator (the raw bytes land in out->bytes) and
 * it holds the epoch of the sequence's first byte (out->start_epoch).
 * Its contents are meaningful only when the call returns 1.
 *
 * Returns:
 *    1  a complete frame was written to *out
 *    0  more bytes are needed (the sequence is still incomplete)
 *   <0  the byte stream is not a valid Set 1 sequence: a negative errno
 *       (EILSEQ) is returned, the partial sequence — including the bytes
 *       already consumed as part of it — is dropped, and the framer
 *       recovers to a sequence boundary.  No frame is written, so a
 *       corrupt sequence never yields a fake Shift / Ctrl.
 */
int input_ps2_frame_feed(input_ps2_frame_state_t *st, uint8_t byte,
                         uint64_t epoch, input_ps2_frame_t *out);

#endif
