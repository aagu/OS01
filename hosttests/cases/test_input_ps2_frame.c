/*
 * test_input_ps2_frame.c — pure PS/2 Set 1 scan-sequence framer
 *
 * GUI-2 plan Task 2.  The framer (kernel/driver/input_ps2.c) turns a PS/2
 * Set 1 byte stream into COMPLETE scan sequences.  It is deliberately
 * pure — no locks, no device IO, no TTY, no display, no physical key
 * bitmap — so this hosttest needs no device mock at all: it links the
 * REAL kernel/driver/input_ps2.c and drives it byte by byte.
 *
 * Contract under test (design spec §4.3 / §6.1):
 *   - a complete sequence is the routing unit: {E0,1D} yields the right
 *     Ctrl frame only after the SECOND byte, never after the E0 prefix;
 *   - {E0,2A,E0,37} and {E0,B7,E0,AA} each yield exactly ONE special
 *     frame (PrintScreen make / break);
 *   - the 6-byte Pause sequence yields exactly ONE special frame;
 *   - start_epoch is the epoch of the FIRST byte of the sequence, even
 *     when the epoch changes at every fragment boundary;
 *   - a corrupt special sequence never produces a fake Shift;
 *   - ordinary 1E / 9E yield the A make / A break frames.
 */
#include <test_framework.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <driver/input_ps2.h>

/* ── Sequence table ──────────────────────────────────────────────
 *
 * Every legal sequence the framer recognises, with the frame it must
 * produce.  `physical_key` is the Set 1 code for ordinary keys and
 * 0x100|code for E0-prefixed keys; the two special keys carry no
 * ordinary key identity at all (0) so that they can never be mistaken
 * for Shift / Ctrl.
 */
typedef struct {
    const char *name;
    uint8_t     bytes[INPUT_PS2_FRAME_MAX_BYTES];
    uint8_t     length;
    uint8_t     is_special;
    uint8_t     is_make;
    uint16_t    physical_key;
} seq_case_t;

static const seq_case_t g_sequences[] = {
    { "A make",             { 0x1E },                    1, 0, 1, 0x01E },
    { "A break",            { 0x9E },                    1, 0, 0, 0x01E },
    { "left shift make",    { 0x2A },                    1, 0, 1, 0x02A },
    { "right ctrl make",    { 0xE0, 0x1D },              2, 0, 1, 0x11D },
    { "right ctrl break",   { 0xE0, 0x9D },              2, 0, 0, 0x11D },
    { "right alt make",     { 0xE0, 0x38 },              2, 0, 1, 0x138 },
    { "PrintScreen make",   { 0xE0, 0x2A, 0xE0, 0x37 }, 4, 1, 1, 0x000 },
    { "PrintScreen break",  { 0xE0, 0xB7, 0xE0, 0xAA }, 4, 1, 0, 0x000 },
    { "Pause",              { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 }, 6, 1, 1, 0x000 },
};

#define SEQ_COUNT ((int)(sizeof(g_sequences) / sizeof(g_sequences[0])))

/* Modifier Set 1 codes a special frame must never impersonate. */
static const uint16_t g_modifier_codes[] = { 0x02A, 0x036, 0x01D, 0x038 };
#define MODIFIER_COUNT ((int)(sizeof(g_modifier_codes) / sizeof(g_modifier_codes[0])))

/* ── Framer fixture ─────────────────────────────────────────── */

typedef struct {
    input_ps2_frame_state_t state;
    input_ps2_frame_t       frame;   /* one buffer for the whole sequence */
    int                     frames;  /* complete frames seen so far */
} framer_t;

static void framer_init(framer_t *f)
{
    input_ps2_frame_reset(&f->state);
    memset(&f->frame, 0, sizeof(f->frame));
    f->frames = 0;
}

/* Feed one byte.  Returns the framer's return value; counts the
 * complete frames that were produced. */
static int framer_feed(framer_t *f, uint8_t byte, uint64_t epoch)
{
    int rc = input_ps2_frame_feed(&f->state, byte, epoch, &f->frame);
    if (rc == 1)
        f->frames++;
    return rc;
}

/* ── Tests ───────────────────────────────────────────────────── */

/* Every legal sequence frames into exactly one frame, and the frame is
 * produced only after the LAST byte of the sequence. */
TEST_FUNC(test_sequences_frame_completely) {
    for (int i = 0; i < SEQ_COUNT; i++) {
        const seq_case_t *c = &g_sequences[i];
        framer_t f;
        framer_init(&f);

        for (int b = 0; b < c->length; b++) {
            int rc = framer_feed(&f, c->bytes[b], 100u + (uint64_t)b);
            if (b < c->length - 1) {
                /* still incomplete: no frame may be published yet */
                assert_eq(0, rc);
                assert_eq(0, f.frames);
            } else {
                assert_eq(1, rc);
            }
        }

        assert_eq(1, f.frames);
        assert_eq(c->length, f.frame.length);
        assert_eq(c->is_special, f.frame.is_special);
        assert_eq(c->is_make, f.frame.is_make);
        assert_eq(c->physical_key, f.frame.physical_key);
        assert_mem_eq(c->bytes, f.frame.bytes, c->length);
    }
}

/* The E0 prefix alone never yields a frame: {E0,1D} is the right Ctrl
 * make and becomes visible only after the second byte. */
TEST_FUNC(test_e0_prefix_emits_only_after_second_byte) {
    framer_t f;
    framer_init(&f);

    assert_eq(0, framer_feed(&f, 0xE0, 7));
    assert_eq(0, f.frames);
    /* A bare E0 prefix is not a key: no physical identity may leak. */
    assert_eq(0, f.frames);

    assert_eq(1, framer_feed(&f, 0x1D, 8));
    assert_eq(1, f.frames);
    assert_eq(0x11D, f.frame.physical_key);
    assert_eq(1, f.frame.is_make);
    assert_eq(0, f.frame.is_special);
    assert_eq(2, f.frame.length);
    assert_eq(0xE0, f.frame.bytes[0]);
    assert_eq(0x1D, f.frame.bytes[1]);
}

/* The two special sequences produce exactly ONE special frame each and
 * never impersonate a modifier key. */
TEST_FUNC(test_special_sequences_produce_one_special_frame) {
    for (int i = 0; i < SEQ_COUNT; i++) {
        const seq_case_t *c = &g_sequences[i];
        if (!c->is_special)
            continue;

        framer_t f;
        framer_init(&f);
        for (int b = 0; b < c->length; b++) {
            int rc = framer_feed(&f, c->bytes[b], 200u + (uint64_t)b);
            assert_eq(b < c->length - 1 ? 0 : 1, rc);
        }
        assert_eq(1, f.frames);
        assert_eq(1, f.frame.is_special);

        /* A special frame must never look like Shift / Ctrl / Alt. */
        for (int m = 0; m < MODIFIER_COUNT; m++)
            assert_true(f.frame.physical_key != g_modifier_codes[m]);
    }
}

/* The 6-byte Pause sequence is framed as ONE special frame. */
TEST_FUNC(test_pause_sequence_is_one_special_frame) {
    static const uint8_t pause[6] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    framer_t f;
    framer_init(&f);

    for (int b = 0; b < 5; b++) {
        assert_eq(0, framer_feed(&f, pause[b], 300u + (uint64_t)b));
        assert_eq(0, f.frames);
    }
    assert_eq(1, framer_feed(&f, pause[5], 305));
    assert_eq(1, f.frames);
    assert_eq(1, f.frame.is_special);
    assert_eq(6, f.frame.length);
    assert_mem_eq(pause, f.frame.bytes, 6);
}

/* start_epoch is the epoch of the FIRST byte of the sequence, even when
 * the epoch changes at every fragment boundary. */
TEST_FUNC(test_epoch_comes_from_first_byte) {
    for (int i = 0; i < SEQ_COUNT; i++) {
        const seq_case_t *c = &g_sequences[i];
        framer_t f;
        framer_init(&f);

        /* every byte carries a different epoch */
        for (int b = 0; b < c->length; b++) {
            uint64_t epoch = 1000u + (uint64_t)b * 17u;
            int rc = framer_feed(&f, c->bytes[b], epoch);
            assert_eq(b < c->length - 1 ? 0 : 1, rc);
        }
        assert_eq(1000u, f.frame.start_epoch);
    }
}

/* The same, split at every fragment boundary: the epoch of the first
 * byte survives the whole sequence. */
TEST_FUNC(test_epoch_survives_every_fragment_boundary) {
    for (int i = 0; i < SEQ_COUNT; i++) {
        const seq_case_t *c = &g_sequences[i];
        for (int split = 1; split < c->length; split++) {
            framer_t f;
            framer_init(&f);

            for (int b = 0; b < split; b++) {
                int rc = framer_feed(&f, c->bytes[b], 4242u);
                assert_eq(0, rc);
                assert_eq(0, f.frames);
            }
            for (int b = split; b < c->length; b++) {
                uint64_t epoch = 9000u + (uint64_t)b;
                int rc = framer_feed(&f, c->bytes[b], epoch);
                assert_eq(b < c->length - 1 ? 0 : 1, rc);
            }
            assert_eq(1, f.frames);
            assert_eq(4242u, f.frame.start_epoch);
        }
    }
}

/* A corrupt special sequence reports an error and never emits a fake
 * Shift: the bytes already consumed as part of the special sequence are
 * not re-interpreted as standalone modifier keys. */
TEST_FUNC(test_corrupt_special_emits_no_fake_shift) {
    framer_t f;
    framer_init(&f);

    /* E0 2A opens the PrintScreen make sequence ... */
    assert_eq(0, framer_feed(&f, 0xE0, 10));
    assert_eq(0, framer_feed(&f, 0x2A, 11));
    assert_eq(0, f.frames);
    /* ... but the sequence is broken here. */
    assert_eq(-EILSEQ, framer_feed(&f, 0x38, 12));
    assert_eq(0, f.frames);

    /* The framer recovered to a sequence boundary: the next key frames
     * normally and no Shift ever appeared. */
    assert_eq(1, framer_feed(&f, 0x1E, 13));
    assert_eq(1, f.frames);
    assert_eq(0x01E, f.frame.physical_key);
    assert_eq(0, f.frame.is_special);
}

/* A corrupt Pause sequence is an error too, and the consumed bytes do
 * not resurface as ordinary keys. */
TEST_FUNC(test_corrupt_pause_emits_no_fake_keys) {
    framer_t f;
    framer_init(&f);

    assert_eq(0, framer_feed(&f, 0xE1, 20));
    assert_eq(0, framer_feed(&f, 0x1D, 21));
    assert_eq(0, framer_feed(&f, 0x45, 22));
    /* the Pause sequence needs E1 here, not 1E */
    assert_eq(-EILSEQ, framer_feed(&f, 0x1E, 23));
    assert_eq(0, f.frames);

    /* back at a clean boundary */
    assert_eq(1, framer_feed(&f, 0x1E, 24));
    assert_eq(1, f.frames);
    assert_eq(0x01E, f.frame.physical_key);
}

/* An unrecognized E0 E0 is corruption (E0 2A E0 37 is the legal double
 * E0 form, and it is recognized as such). */
TEST_FUNC(test_corrupt_e0_prefix_is_reported) {
    framer_t f;
    framer_init(&f);

    assert_eq(0, framer_feed(&f, 0xE0, 30));
    assert_eq(-EILSEQ, framer_feed(&f, 0xE0, 31));
    assert_eq(0, f.frames);

    /* recovered */
    assert_eq(1, framer_feed(&f, 0x1E, 32));
    assert_eq(1, f.frames);
    assert_eq(0x01E, f.frame.physical_key);
}

/* reset() drops a partial sequence: the stale prefix must not leak into
 * the next key. */
TEST_FUNC(test_reset_drops_partial_sequence) {
    framer_t f;
    framer_init(&f);

    assert_eq(0, framer_feed(&f, 0xE0, 40));
    input_ps2_frame_reset(&f.state);
    f.frames = 0;

    assert_eq(1, framer_feed(&f, 0x1E, 41));
    assert_eq(1, f.frames);
    assert_eq(0x01E, f.frame.physical_key);
    assert_eq(1, f.frame.length);
}

/* The persistent state stays within the 6-byte budget of the brief. */
TEST_FUNC(test_state_stays_small) {
    assert_true(sizeof(input_ps2_frame_state_t) <= 6);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_sequences_frame_completely),
    TEST_ENTRY(test_e0_prefix_emits_only_after_second_byte),
    TEST_ENTRY(test_special_sequences_produce_one_special_frame),
    TEST_ENTRY(test_pause_sequence_is_one_special_frame),
    TEST_ENTRY(test_epoch_comes_from_first_byte),
    TEST_ENTRY(test_epoch_survives_every_fragment_boundary),
    TEST_ENTRY(test_corrupt_special_emits_no_fake_shift),
    TEST_ENTRY(test_corrupt_pause_emits_no_fake_keys),
    TEST_ENTRY(test_corrupt_e0_prefix_is_reported),
    TEST_ENTRY(test_reset_drops_partial_sequence),
    TEST_ENTRY(test_state_stays_small),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
