/*
 * test_gui_input_decode.c — native input API + pure userspace PS/2 decoder
 *
 * GUI-2 plan Task 5.  The decoder (libgui/src/input_ps2.c) is PURE — no
 * device access, no locks, no LVGL — so this hosttest needs no device
 * mock: it host-compiles the REAL production decoder and drives it byte
 * by byte, exactly like the kernel-side framer test of Task 2.
 *
 * Contract under test (design spec §5.1 / §6.1):
 *   - US ASCII layout: letters use Shift XOR Caps, digits/punctuation
 *     use Shift only;
 *   - left and right Shift/Ctrl/Alt are tracked separately and keep
 *     distinct physical identities (0x11D = right Ctrl, ...);
 *   - an UP event carries the identity of its DOWN and codepoint == 0;
 *   - a repeated make (typematic) produces NO event and does not flip
 *     Caps again;
 *   - PrintScreen (E0 2A E0 37 / E0 B7 E0 AA) and Pause
 *     (E1 1D 45 E1 9D C5) are swallowed WITHOUT an event and WITHOUT
 *     polluting the Shift/Ctrl state — the ghost-Shift failure the
 *     whole plan exists to prevent;
 *   - a truncated prefix survives 99 ms and is dropped at 100 ms;
 *   - an unknown scan code reports the physical identity but no
 *     character;
 *   - KP slash / KP Enter are supported as their own physical keys;
 *   - PageUp / PageDown / Insert and the numpad NumLock layout report
 *     the physical key identity ONLY — they never masquerade as text
 *     or as arrow keys.
 */
#include <test_framework.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <gui/gui_input.h>
#include <gui_input_ps2.h>

/* ── Decoder fixture ─────────────────────────────────────────── */

typedef struct {
    gui_ps2_state_t state;
    gui_key_event_t event;
    int             events;   /* events produced so far */
} decoder_t;

static void dec_init(decoder_t *d, uint16_t modifiers)
{
    gui_ps2_init(&d->state, modifiers);
    memset(&d->event, 0, sizeof(d->event));
    d->events = 0;
}

/* Feed one byte.  Returns 1 when the decoder produced an event. */
static int dec_feed(decoder_t *d, uint8_t byte, uint64_t now_ms)
{
    int rc = gui_ps2_feed(&d->state, byte, now_ms, &d->event);

    assert_true(rc == 0 || rc == 1);
    if (rc == 1) {
        d->events++;
        return 1;
    }
    return 0;
}

/* Feed a whole sequence and count the events it produced. */
static int dec_feed_seq(decoder_t *d, const uint8_t *bytes, size_t len,
                        uint64_t now_ms)
{
    int produced = 0;

    for (size_t i = 0; i < len; i++) {
        if (dec_feed(d, bytes[i], now_ms + (uint64_t)i))
            produced++;
    }
    return produced;
}

/* Table rows carry a name so a failure points at the exact case; the
 * framework's assert_eq only reports file:line. */
static void assert_case_eq(const char *name, long expected, long actual)
{
    if (expected != actual)
        printf("  [case] %s: expected %ld, got %ld\n", name, expected, actual);
    assert_eq(expected, actual);
}

/* ── 1. The frozen public API surface ────────────────────────── */

/* The native action values and the modifier bit layout are FROZEN: the
 * LVGL adapter (Task 7) and the application code depend on them. */
TEST_FUNC(test_public_action_values_are_frozen) {
    assert_eq(0, (int)GUI_KEY_DOWN);
    assert_eq(1, (int)GUI_KEY_UP);
    assert_eq(2, (int)GUI_KEY_REPEAT);
}

/* The modifier bits are frozen in this exact order. */
TEST_FUNC(test_public_modifier_bits_are_frozen) {
    assert_eq(1 << 0, (int)GUI_MOD_LSHIFT);
    assert_eq(1 << 1, (int)GUI_MOD_RSHIFT);
    assert_eq(1 << 2, (int)GUI_MOD_LCTRL);
    assert_eq(1 << 3, (int)GUI_MOD_RCTRL);
    assert_eq(1 << 4, (int)GUI_MOD_LALT);
    assert_eq(1 << 5, (int)GUI_MOD_RALT);
    assert_eq(1 << 6, (int)GUI_MOD_CAPS);
}

/* The native event types are plain fixed-width fields: no LVGL key
 * constant or LVGL type ever appears in them. */
TEST_FUNC(test_public_types_stay_native) {
    gui_key_event_t key;
    gui_pointer_event_t ptr;

    memset(&key, 0, sizeof(key));
    memset(&ptr, 0, sizeof(ptr));

    assert_eq(0, (int)key.action);
    assert_eq(0, (long)key.physical_key);
    assert_eq(0, (long)key.modifiers);
    assert_eq(0, (long)key.codepoint);

    assert_eq(0, (long)ptr.x);
    assert_eq(0, (long)ptr.y);
    assert_eq(0, (int)ptr.buttons);
    assert_eq(0, (int)ptr.changed_buttons);
    assert_eq(0, (int)ptr.wheel);
}

/* ── 2. US ASCII translation ─────────────────────────────────── */

typedef struct {
    const char *name;
    uint8_t     mods[2];      /* modifier make codes held while pressing */
    uint8_t     mod_count;
    uint8_t     code;         /* the key's Set 1 make code */
    uint32_t    codepoint;    /* the expected character */
    uint16_t    expect_mods;  /* the expected modifier snapshot */
} char_case_t;

static const char_case_t g_char_cases[] = {
    /* plain letters */
    { "a",              { 0x00, 0x00 }, 0, 0x1E, 97,  0 },
    { "z",              { 0x00, 0x00 }, 0, 0x2C, 122, 0 },
    /* shift alone */
    { "Shift+a",        { 0x2A, 0x00 }, 1, 0x1E, 65,  GUI_MOD_LSHIFT },
    { "RShift+a",       { 0x36, 0x00 }, 1, 0x1E, 65,  GUI_MOD_RSHIFT },
    { "Shift+z",        { 0x2A, 0x00 }, 1, 0x2C, 90,  GUI_MOD_LSHIFT },
    /* caps alone */
    { "Caps+a",         { 0x3A, 0x00 }, 1, 0x1E, 65,  GUI_MOD_CAPS },
    { "Caps+z",         { 0x3A, 0x00 }, 1, 0x2C, 90,  GUI_MOD_CAPS },
    /* caps XOR shift -> back to lower case */
    { "Caps+Shift+a",   { 0x3A, 0x2A }, 2, 0x1E, 97,  GUI_MOD_CAPS | GUI_MOD_LSHIFT },
    { "Caps+RShift+a",  { 0x3A, 0x36 }, 2, 0x1E, 97,  GUI_MOD_CAPS | GUI_MOD_RSHIFT },
    /* digits and punctuation: shift only, caps never applies */
    { "1",              { 0x00, 0x00 }, 0, 0x02, 49, 0 },
    { "Shift+1",        { 0x2A, 0x00 }, 1, 0x02, 33,  GUI_MOD_LSHIFT },
    { "0",              { 0x00, 0x00 }, 0, 0x0B, 48, 0 },
    { "Shift+0",        { 0x2A, 0x00 }, 1, 0x0B, 41,  GUI_MOD_LSHIFT },
    { "minus",         { 0x00, 0x00 }, 0, 0x0C, 45, 0 },
    { "Shift+minus",   { 0x2A, 0x00 }, 1, 0x0C, 95,  GUI_MOD_LSHIFT },
    { "semicolon",     { 0x00, 0x00 }, 0, 0x27, 59, 0 },
    { "Shift+semicolon", { 0x2A, 0x00 }, 1, 0x27, 58, GUI_MOD_LSHIFT },
    { "comma",         { 0x00, 0x00 }, 0, 0x33, 44, 0 },
    { "Shift+comma",   { 0x2A, 0x00 }, 1, 0x33, 60,  GUI_MOD_LSHIFT },
    { "slash",         { 0x00, 0x00 }, 0, 0x35, 47, 0 },
    { "Shift+slash",   { 0x2A, 0x00 }, 1, 0x35, 63,  GUI_MOD_LSHIFT },
    { "backquote",     { 0x00, 0x00 }, 0, 0x29, 96, 0 },
    { "Shift+backquote", { 0x2A, 0x00 }, 1, 0x29, 126, GUI_MOD_LSHIFT },
    /* space is not affected by shift */
    { "space",         { 0x00, 0x00 }, 0, 0x39, 32, 0 },
    { "Shift+space",   { 0x2A, 0x00 }, 1, 0x39, 32,  GUI_MOD_LSHIFT },
};

#define CHAR_CASE_COUNT ((int)(sizeof(g_char_cases) / sizeof(g_char_cases[0])))

/* The table above IS the US ASCII layout contract: letters use
 * Shift XOR Caps, everything else uses Shift only. */
TEST_FUNC(test_ascii_translation_table) {
    for (int i = 0; i < CHAR_CASE_COUNT; i++) {
        const char_case_t *c = &g_char_cases[i];
        decoder_t d;
        uint64_t now = 1000;

        dec_init(&d, 0);

        for (int m = 0; m < c->mod_count; m++)
            assert_true(dec_feed(&d, c->mods[m], now++) == 1);

        assert_true(dec_feed(&d, c->code, now++) == 1);
        assert_eq((int)GUI_KEY_DOWN, (int)d.event.action);
        assert_case_eq(c->name, (long)c->codepoint, (long)d.event.codepoint);
        assert_case_eq(c->name, (long)c->expect_mods, (long)d.event.modifiers);
        assert_case_eq(c->name, (long)c->code, (long)d.event.physical_key);
    }
}

/* ── 3. Modifier identity ────────────────────────────────────── */

typedef struct {
    const char *name;
    uint8_t     bytes[2];
    uint8_t     length;
    uint16_t    physical_key;
    uint16_t    modifier;
} modifier_case_t;

static const modifier_case_t g_modifier_cases[] = {
    { "left shift",  { 0x2A, 0x00 }, 1, 0x02A, GUI_MOD_LSHIFT },
    { "right shift", { 0x36, 0x00 }, 1, 0x036, GUI_MOD_RSHIFT },
    { "left ctrl",   { 0x1D, 0x00 }, 1, 0x01D, GUI_MOD_LCTRL },
    { "right ctrl",  { 0xE0, 0x1D }, 2, 0x11D, GUI_MOD_RCTRL },
    { "left alt",    { 0x38, 0x00 }, 1, 0x038, GUI_MOD_LALT },
    { "right alt",   { 0xE0, 0x38 }, 2, 0x138, GUI_MOD_RALT },
};

#define MODIFIER_CASE_COUNT \
    ((int)(sizeof(g_modifier_cases) / sizeof(g_modifier_cases[0])))

/* Left and right Shift/Ctrl/Alt are separate physical keys with
 * separate modifier bits. */
TEST_FUNC(test_modifier_keys_keep_distinct_identity) {
    for (int i = 0; i < MODIFIER_CASE_COUNT; i++) {
        const modifier_case_t *c = &g_modifier_cases[i];
        decoder_t d;

        dec_init(&d, 0);
        assert_true(dec_feed_seq(&d, c->bytes, c->length, 100) == 1);

        assert_eq((int)GUI_KEY_DOWN, (int)d.event.action);
        assert_case_eq(c->name, (long)c->physical_key, (long)d.event.physical_key);
        assert_case_eq(c->name, (long)c->modifier, (long)d.event.modifiers);
        assert_eq(0, d.event.codepoint);
    }
}

/* Holding both sides keeps BOTH bits — the sides never collapse. */
TEST_FUNC(test_left_and_right_modifiers_are_independent) {
    decoder_t d;

    dec_init(&d, 0);

    assert_true(dec_feed(&d, 0x1D, 10) == 1);   /* left ctrl */
    assert_eq(GUI_MOD_LCTRL, d.event.modifiers);

    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x1D }, 2, 11) == 1);
    assert_eq(0x11D, d.event.physical_key);
    assert_eq(GUI_MOD_LCTRL | GUI_MOD_RCTRL, d.event.modifiers);

    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x38 }, 2, 12) == 1);
    assert_eq(0x138, d.event.physical_key);
    assert_eq(GUI_MOD_LCTRL | GUI_MOD_RCTRL | GUI_MOD_RALT, d.event.modifiers);

    /* Releasing the right Ctrl clears only its own bit. */
    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x9D }, 2, 13) == 1);
    assert_eq((int)GUI_KEY_UP, (int)d.event.action);
    assert_eq(0x11D, d.event.physical_key);
    assert_eq(GUI_MOD_LCTRL | GUI_MOD_RALT, d.event.modifiers);
}

/* Ctrl / Alt combinations never produce a character. */
TEST_FUNC(test_ctrl_and_alt_combinations_have_no_codepoint) {
    static const uint8_t ctrl[] = { 0x1D };
    static const uint8_t alt[]  = { 0x38 };

    for (int which = 0; which < 2; which++) {
        decoder_t d;

        dec_init(&d, 0);
        assert_true(dec_feed_seq(&d, which == 0 ? ctrl : alt, 1, 100) == 1);

        assert_true(dec_feed(&d, 0x1E, 101) == 1);   /* 'a' */
        assert_eq(0x01E, d.event.physical_key);
        assert_eq(0, d.event.codepoint);
    }
}

/* ── 4. Edges: UP, repeated make, Caps ───────────────────────── */

/* An UP event carries the identity of its DOWN and never a character. */
TEST_FUNC(test_up_event_has_no_codepoint) {
    decoder_t d;

    dec_init(&d, 0);

    assert_true(dec_feed(&d, 0x1E, 100) == 1);
    assert_eq((int)GUI_KEY_DOWN, (int)d.event.action);
    assert_eq(0x01E, d.event.physical_key);
    assert_eq(97, d.event.codepoint);

    assert_true(dec_feed(&d, 0x9E, 101) == 1);
    assert_eq((int)GUI_KEY_UP, (int)d.event.action);
    assert_eq(0x01E, d.event.physical_key);
    assert_eq(0, d.event.codepoint);
}

/* The UP of an E0 key keeps the E0 identity. */
TEST_FUNC(test_up_event_keeps_e0_identity) {
    decoder_t d;

    dec_init(&d, 0);

    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x1D }, 2, 100) == 1);
    assert_eq(0x11D, d.event.physical_key);

    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x9D }, 2, 101) == 1);
    assert_eq((int)GUI_KEY_UP, (int)d.event.action);
    assert_eq(0x11D, d.event.physical_key);
    assert_eq(0, d.event.codepoint);
}

/* A repeated make (typematic) is NOT a new edge: no event, and the
 * held state stays. */
TEST_FUNC(test_repeated_make_produces_no_event) {
    decoder_t d;

    dec_init(&d, 0);

    assert_true(dec_feed(&d, 0x1E, 100) == 1);
    assert_eq(1, d.events);

    assert_true(dec_feed(&d, 0x1E, 120) == 0);
    assert_true(dec_feed(&d, 0x1E, 140) == 0);
    assert_eq(1, d.events);

    /* The key is still held: its UP is still reported. */
    assert_true(dec_feed(&d, 0x9E, 160) == 1);
    assert_eq(2, d.events);
    assert_eq((int)GUI_KEY_UP, (int)d.event.action);
}

/* Caps flips only on a genuine up -> down edge. */
TEST_FUNC(test_caps_flips_only_on_down_edge) {
    decoder_t d;

    dec_init(&d, 0);

    /* Caps down -> the latch turns on. */
    assert_true(dec_feed(&d, 0x3A, 100) == 1);
    assert_eq(0x03A, d.event.physical_key);
    assert_eq(GUI_MOD_CAPS, d.event.modifiers);

    /* The typematic repeat must NOT flip it again. */
    assert_true(dec_feed(&d, 0x3A, 120) == 0);
    assert_eq(GUI_MOD_CAPS, d.event.modifiers);

    /* 'a' is upper case now. */
    assert_true(dec_feed(&d, 0x1E, 130) == 1);
    assert_eq(65, d.event.codepoint);

    /* Caps up -> the latch stays on. */
    assert_true(dec_feed(&d, 0xBA, 140) == 1);
    assert_eq((int)GUI_KEY_UP, (int)d.event.action);
    assert_eq(GUI_MOD_CAPS, d.event.modifiers);

    /* Release 'a' so the next press is a real down edge. */
    assert_true(dec_feed(&d, 0x9E, 145) == 1);

    /* Caps down again -> the latch turns OFF (a real edge). */
    assert_true(dec_feed(&d, 0x3A, 150) == 1);
    assert_eq(0, d.event.modifiers);

    assert_true(dec_feed(&d, 0x1E, 160) == 1);
    assert_eq(97, d.event.codepoint);
}

/* ── 5. The special sequences must not pollute the modifiers ── */

/* PrintScreen make (E0 2A E0 37) and break (E0 B7 E0 AA) are swallowed:
 * no event at all and NO latched Shift. */
TEST_FUNC(test_printscreen_does_not_pollute_modifiers) {
    static const uint8_t ps_make[]  = { 0xE0, 0x2A, 0xE0, 0x37 };
    static const uint8_t ps_break[] = { 0xE0, 0xB7, 0xE0, 0xAA };
    decoder_t d;

    dec_init(&d, 0);

    assert_eq(0, dec_feed_seq(&d, ps_make, 4, 100));
    assert_eq(0, d.events);
    assert_eq(0, d.event.modifiers);

    assert_eq(0, dec_feed_seq(&d, ps_break, 4, 200));
    assert_eq(0, d.events);
    assert_eq(0, d.event.modifiers);

    /* The ghost Shift is gone: 'a' is still lower case and no Shift
     * bit is reported. */
    assert_true(dec_feed(&d, 0x1E, 300) == 1);
    assert_eq(97, d.event.codepoint);
    assert_eq(0, d.event.modifiers);
}

/* Pause (E1 1D 45 E1 9D C5) is swallowed as a whole and must not latch
 * the fake Ctrl. */
TEST_FUNC(test_pause_does_not_pollute_modifiers) {
    static const uint8_t pause[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    decoder_t d;

    dec_init(&d, 0);

    assert_eq(0, dec_feed_seq(&d, pause, 6, 100));
    assert_eq(0, d.events);
    assert_eq(0, d.event.modifiers);

    /* No Ctrl leaked: 'a' still produces a character. */
    assert_true(dec_feed(&d, 0x1E, 200) == 1);
    assert_eq(97, d.event.codepoint);
    assert_eq(0, d.event.modifiers);
}

/* The special sequences are also swallowed when they arrive in
 * fragments — a truncated prefix must never leak a fake modifier. */
TEST_FUNC(test_special_sequences_are_swallowed_fragment_by_fragment) {
    static const uint8_t ps_make[] = { 0xE0, 0x2A, 0xE0, 0x37 };
    decoder_t d;

    dec_init(&d, 0);

    for (size_t i = 0; i < sizeof(ps_make); i++) {
        assert_true(dec_feed(&d, ps_make[i], 100 + (uint64_t)i) == 0);
        assert_eq(0, d.events);
        assert_eq(0, d.event.modifiers);
    }
}

/* ── 6. The E0 framing ───────────────────────────────────────── */

/* An E0 sequence produces its event only after the LAST byte, and
 * every fragment point keeps the state clean. */
TEST_FUNC(test_e0_sequence_every_fragment_point) {
    static const uint8_t right_ctrl[] = { 0xE0, 0x1D };
    decoder_t d;

    for (size_t split = 1; split < sizeof(right_ctrl); split++) {
        dec_init(&d, 0);

        /* The prefix alone never produces an event. */
        for (size_t i = 0; i < split; i++)
            assert_true(dec_feed(&d, right_ctrl[i], 100 + (uint64_t)i) == 0);
        assert_eq(0, d.events);

        /* The tail completes the sequence. */
        for (size_t i = split; i < sizeof(right_ctrl); i++)
            assert_true(dec_feed(&d, right_ctrl[i], 100 + (uint64_t)i) == 1);

        assert_eq(1, d.events);
        assert_eq((int)GUI_KEY_DOWN, (int)d.event.action);
        assert_eq(0x11D, d.event.physical_key);
        assert_eq(GUI_MOD_RCTRL, d.event.modifiers);
    }
}

/* A truncated prefix survives 99 ms and is dropped at 100 ms. */
TEST_FUNC(test_truncated_prefix_expires_after_100ms) {
    decoder_t d;

    /* 99 ms: the prefix is still pending, so the tail is decoded as
     * the right Ctrl. */
    dec_init(&d, 0);
    assert_true(dec_feed(&d, 0xE0, 0) == 0);
    gui_ps2_expire(&d.state, 99);
    assert_true(dec_feed(&d, 0x1D, 99) == 1);
    assert_eq(0x11D, d.event.physical_key);
    assert_eq(GUI_MOD_RCTRL, d.event.modifiers);

    /* 100 ms: the prefix is gone, so the tail is a bare left Ctrl. */
    dec_init(&d, 0);
    assert_true(dec_feed(&d, 0xE0, 0) == 0);
    gui_ps2_expire(&d.state, 100);
    assert_true(dec_feed(&d, 0x1D, 100) == 1);
    assert_eq(0x01D, d.event.physical_key);
    assert_eq(GUI_MOD_LCTRL, d.event.modifiers);
}

/* A prefix that timed out must not resurrect itself when the late
 * byte finally arrives. */
TEST_FUNC(test_late_byte_after_expired_prefix_starts_a_fresh_sequence) {
    decoder_t d;

    dec_init(&d, 0);
    assert_true(dec_feed(&d, 0xE0, 0) == 0);
    gui_ps2_expire(&d.state, 500);
    assert_true(dec_feed(&d, 0x1D, 500) == 1);
    assert_eq(0x01D, d.event.physical_key);
}

/* ── 7. Keys without a character ─────────────────────────────── */

/* An unknown scan code reports its physical identity and no
 * character. */
TEST_FUNC(test_unknown_code_has_no_character) {
    decoder_t d;

    dec_init(&d, 0);

    assert_true(dec_feed(&d, 0x77, 100) == 1);
    assert_eq(0x077, d.event.physical_key);
    assert_eq(0, d.event.codepoint);
    assert_eq((int)GUI_KEY_DOWN, (int)d.event.action);
}

/* KP slash and KP Enter are supported as their own physical keys and
 * never collide with the main keyboard '/' and Enter. */
TEST_FUNC(test_keypad_slash_and_enter_are_supported) {
    decoder_t d;

    dec_init(&d, 0);

    /* KP slash (E0 35) */
    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x35 }, 2, 100) == 1);
    assert_eq(0x135, d.event.physical_key);
    assert_eq(0, d.event.codepoint);

    /* KP Enter (E0 1C) */
    assert_true(dec_feed_seq(&d, (const uint8_t[]){ 0xE0, 0x1C }, 2, 101) == 1);
    assert_eq(0x11C, d.event.physical_key);
    assert_eq(0, d.event.codepoint);

    /* The main keyboard '/' is a character and a different key. */
    assert_true(dec_feed(&d, 0x35, 102) == 1);
    assert_eq(0x035, d.event.physical_key);
    assert_eq(47, d.event.codepoint);

    /* The main keyboard Enter is a different key as well. */
    assert_true(dec_feed(&d, 0x1C, 103) == 1);
    assert_eq(0x01C, d.event.physical_key);
    assert_eq(0, d.event.codepoint);
}

typedef struct {
    const char *name;
    uint8_t     bytes[2];
    uint8_t     length;
    uint16_t    physical_key;
} identity_only_case_t;

/* PageUp / PageDown / Insert and the numpad NumLock layout report the
 * physical key identity ONLY: no text, and never an arrow key. */
static const identity_only_case_t g_identity_only_cases[] = {
    { "numpad 7",         { 0x47, 0x00 }, 1, 0x047 },
    { "numpad 4",         { 0x4B, 0x00 }, 1, 0x04B },
    { "numpad 1",         { 0x4F, 0x00 }, 1, 0x04F },
    { "numpad 0",         { 0x52, 0x00 }, 1, 0x052 },
    { "numpad PageUp",    { 0x49, 0x00 }, 1, 0x049 },
    { "numpad PageDown",  { 0x51, 0x00 }, 1, 0x051 },
    { "PageUp",           { 0xE0, 0x49 }, 2, 0x149 },
    { "PageDown",         { 0xE0, 0x51 }, 2, 0x151 },
    { "Insert",           { 0xE0, 0x52 }, 2, 0x152 },
    { "Home",             { 0xE0, 0x47 }, 2, 0x147 },
    { "End",              { 0xE0, 0x4F }, 2, 0x14F },
    { "Delete",           { 0xE0, 0x53 }, 2, 0x153 },
    { "up arrow",         { 0xE0, 0x48 }, 2, 0x148 },
    { "down arrow",       { 0xE0, 0x50 }, 2, 0x150 },
    { "left arrow",       { 0xE0, 0x4B }, 2, 0x14B },
    { "right arrow",      { 0xE0, 0x4D }, 2, 0x14D },
};

#define IDENTITY_ONLY_COUNT \
    ((int)(sizeof(g_identity_only_cases) / sizeof(g_identity_only_cases[0])))

TEST_FUNC(test_navigation_and_numpad_report_physical_only) {
    for (int i = 0; i < IDENTITY_ONLY_COUNT; i++) {
        const identity_only_case_t *c = &g_identity_only_cases[i];
        decoder_t d;

        dec_init(&d, 0);
        assert_true(dec_feed_seq(&d, c->bytes, c->length, 100) == 1);

        assert_eq((int)GUI_KEY_DOWN, (int)d.event.action);
        assert_case_eq(c->name, (long)c->physical_key, (long)d.event.physical_key);
        assert_eq(0, d.event.codepoint);
    }
}

/* The navigation keys must never be confused with a digit: the numpad
 * 7 is not the character '7'. */
TEST_FUNC(test_numpad_digits_are_not_text) {
    decoder_t d;

    dec_init(&d, 0);

    assert_true(dec_feed(&d, 0x47, 100) == 1);   /* numpad 7 */
    assert_eq(0x047, d.event.physical_key);
    assert_eq(0, d.event.codepoint);

    assert_true(dec_feed(&d, 0x0B, 101) == 1);   /* the main keyboard '0' */
    assert_eq(0x00B, d.event.physical_key);
    assert_eq(48, d.event.codepoint);
}

/* ── 8. The decoder contract itself ──────────────────────────── */

/* A NULL state or a NULL output buffer is an argument error. */
TEST_FUNC(test_feed_rejects_null_arguments) {
    decoder_t d;
    gui_key_event_t event;

    dec_init(&d, 0);
    memset(&event, 0, sizeof(event));

    errno = 0;
    assert_eq(-1, gui_ps2_feed(NULL, 0x1E, 0, &event));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(-1, gui_ps2_feed(&d.state, 0x1E, 0, NULL));
    assert_eq(EINVAL, errno);
}

/* The initial modifier snapshot is the state the decoder starts from. */
TEST_FUNC(test_init_applies_initial_modifiers) {
    decoder_t d;

    dec_init(&d, GUI_MOD_CAPS);

    assert_true(dec_feed(&d, 0x1E, 100) == 1);
    assert_eq(65, d.event.codepoint);
    assert_eq(GUI_MOD_CAPS, d.event.modifiers);
}

/* The decoder keeps its state in the caller's frame: it is a plain
 * stack-allocatable object. */
TEST_FUNC(test_decoder_state_is_stack_allocatable) {
    gui_ps2_state_t state;
    gui_key_event_t event;

    memset(&state, 0, sizeof(state));
    memset(&event, 0, sizeof(event));

    gui_ps2_init(&state, 0);
    assert_eq(1, gui_ps2_feed(&state, 0x1E, 0, &event));
    assert_eq(97, event.codepoint);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_public_action_values_are_frozen),
    TEST_ENTRY(test_public_modifier_bits_are_frozen),
    TEST_ENTRY(test_public_types_stay_native),
    TEST_ENTRY(test_ascii_translation_table),
    TEST_ENTRY(test_modifier_keys_keep_distinct_identity),
    TEST_ENTRY(test_left_and_right_modifiers_are_independent),
    TEST_ENTRY(test_ctrl_and_alt_combinations_have_no_codepoint),
    TEST_ENTRY(test_up_event_has_no_codepoint),
    TEST_ENTRY(test_up_event_keeps_e0_identity),
    TEST_ENTRY(test_repeated_make_produces_no_event),
    TEST_ENTRY(test_caps_flips_only_on_down_edge),
    TEST_ENTRY(test_printscreen_does_not_pollute_modifiers),
    TEST_ENTRY(test_pause_does_not_pollute_modifiers),
    TEST_ENTRY(test_special_sequences_are_swallowed_fragment_by_fragment),
    TEST_ENTRY(test_e0_sequence_every_fragment_point),
    TEST_ENTRY(test_truncated_prefix_expires_after_100ms),
    TEST_ENTRY(test_late_byte_after_expired_prefix_starts_a_fresh_sequence),
    TEST_ENTRY(test_unknown_code_has_no_character),
    TEST_ENTRY(test_keypad_slash_and_enter_are_supported),
    TEST_ENTRY(test_navigation_and_numpad_report_physical_only),
    TEST_ENTRY(test_numpad_digits_are_not_text),
    TEST_ENTRY(test_feed_rejects_null_arguments),
    TEST_ENTRY(test_init_applies_initial_modifiers),
    TEST_ENTRY(test_decoder_state_is_stack_allocatable),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
