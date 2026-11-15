/* @os01-test
 * id: test_usb_hid_report
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: nightly
 * link: usb-hid-report
 * scenarios: [drivers.usb.hid_report]
 */
#include "test_framework.h"
#include <driver/usb_hid_report.h>
#include <driver/input_ps2.h>
#include <uapi/mouse.h>
#include <errno.h>
#include <string.h>

static void test_keyboard_boot_diff(void)
{
    usb_hid_keyboard_t kbd;
    input_ps2_frame_t frames[USB_HID_MAX_FRAMES];
    int rc;

    memset(&kbd, 0, sizeof(kbd));

    /* 1. Initial report with Right Ctrl (0x10) and duplicate 'a' (0x04):
     * {0x10, 0, 0x04, 0x04, 0, 0, 0, 0}
     * Must produce Right Ctrl make (E0 1D) followed by 'a' make (1E). */
    const uint8_t r1[8] = { 0x10, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00 };
    rc = usb_hid_keyboard_update(&kbd, r1, sizeof(r1), frames);
    assert_eq(2, rc);
    if (rc == 2) {
        /* Frame 0: Right Ctrl make -> E0 1D */
        assert_eq(2, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0x1D, frames[0].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x1D, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);
        assert_eq(0, frames[0].is_special);

        /* Frame 1: 'a' make -> 1E */
        assert_eq(1, frames[1].length);
        assert_eq(0x1E, frames[1].bytes[0]);
        assert_eq(0x1E, frames[1].physical_key);
        assert_eq(1, frames[1].is_make);
        assert_eq(0, frames[1].is_special);
    }

    /* 2. Repeating the exact same input produces 0 frames. */
    rc = usb_hid_keyboard_update(&kbd, r1, sizeof(r1), frames);
    assert_eq(0, rc);

    /* 3. Rollover report: modifier=0 and array contains 0x01: {0x00, 0, 0x01, 0, 0, 0, 0, 0}.
     * Must preserve previous ordinary key set ('a'), only update modifier.
     * Right Ctrl was released -> only E0 9D. */
    const uint8_t r_rollover[8] = { 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    rc = usb_hid_keyboard_update(&kbd, r_rollover, sizeof(r_rollover), frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(2, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0x9D, frames[0].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x1D, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);
        assert_eq(0, frames[0].is_special);
    }

    /* 4. All-zero recovery report: {0, 0, 0, 0, 0, 0, 0, 0}.
     * Diff against preserved key 'a' -> 'a' break (9E). */
    const uint8_t r_zero[8] = { 0 };
    rc = usb_hid_keyboard_update(&kbd, r_zero, sizeof(r_zero), frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(1, frames[0].length);
        assert_eq(0x9E, frames[0].bytes[0]);
        assert_eq(0x1E, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);
        assert_eq(0, frames[0].is_special);
    }

    /* 5. Invalid lengths (7 and 9 bytes) must return -EINVAL and NOT alter state. */
    const uint8_t r_short[7] = { 0x10, 0, 0x04, 0, 0, 0, 0 };
    const uint8_t r_long[9] = { 0x10, 0, 0x04, 0, 0, 0, 0, 0, 0 };
    usb_hid_keyboard_t saved_kbd = kbd;
    assert_eq(-EINVAL, usb_hid_keyboard_update(&kbd, r_short, 7, frames));
    assert_eq(0, memcmp(&saved_kbd, &kbd, sizeof(kbd)));
    assert_eq(-EINVAL, usb_hid_keyboard_update(&kbd, r_long, 9, frames));
    assert_eq(0, memcmp(&saved_kbd, &kbd, sizeof(kbd)));
    assert_eq(-EINVAL, usb_hid_keyboard_update(NULL, r1, 8, frames));
    assert_eq(-EINVAL, usb_hid_keyboard_update(&kbd, NULL, 8, frames));
    assert_eq(-EINVAL, usb_hid_keyboard_update(&kbd, r1, 8, NULL));
}

static void test_keyboard_release_and_held_inputs(void)
{
    usb_hid_keyboard_t kbd;
    input_ps2_frame_t frames[USB_HID_MAX_FRAMES];
    size_t count;
    int rc;

    memset(&kbd, 0, sizeof(kbd));

    /* Hold 1 modifier (Left Shift = 0x02) and 6 ordinary keys (0x04..0x09: a,b,c,d,e,f) */
    const uint8_t r[8] = { 0x02, 0x00, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    rc = usb_hid_keyboard_update(&kbd, r, sizeof(r), frames);
    assert_eq(7, rc);

    /* Disconnect release: must release all 7 held inputs (6 ordinary keys + 1 modifier) */
    count = usb_hid_keyboard_release(&kbd, frames);
    assert_eq(7, count);
    if (count == 7) {
        /* Ordinary breaks in ascending order of HID usage:
         * 0x04 ('a'): 0x9E
         * 0x05 ('b'): 0xB0
         * 0x06 ('c'): 0xAE
         * 0x07 ('d'): 0xA0
         * 0x08 ('e'): 0x92
         * 0x09 ('f'): 0xA1
         */
        const uint8_t expected_breaks[6] = { 0x9E, 0xB0, 0xAE, 0xA0, 0x92, 0xA1 };
        const uint16_t expected_keys[6] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21 };
        for (int i = 0; i < 6; i++) {
            assert_eq(1, frames[i].length);
            assert_eq(expected_breaks[i], frames[i].bytes[0]);
            assert_eq(expected_keys[i], frames[i].physical_key);
            assert_eq(0, frames[i].is_make);
            assert_eq(0, frames[i].is_special);
        }

        /* Modifier break: Left Shift -> 0xAA */
        assert_eq(1, frames[6].length);
        assert_eq(0xAA, frames[6].bytes[0]);
        assert_eq(0x2A, frames[6].physical_key);
        assert_eq(0, frames[6].is_make);
        assert_eq(0, frames[6].is_special);
    }

    /* State must be completely cleared */
    assert_eq(0, kbd.modifiers);
    assert_eq(0, kbd.key_count);
    for (int i = 0; i < 6; i++) {
        assert_eq(0, kbd.keys[i]);
    }

    /* Further release returns 0 */
    assert_eq(0, usb_hid_keyboard_release(&kbd, frames));
    assert_eq(0, usb_hid_keyboard_release(NULL, frames));
    assert_eq(0, usb_hid_keyboard_release(&kbd, NULL));
}

static void test_caps_printscreen_pause(void)
{
    usb_hid_keyboard_t kbd;
    input_ps2_frame_t frames[USB_HID_MAX_FRAMES];
    int rc;
    size_t count;

    memset(&kbd, 0, sizeof(kbd));

    /* 1. Caps Lock (0x39) */
    const uint8_t r_caps[8] = { 0x00, 0x00, 0x39, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_caps, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(1, frames[0].length);
        assert_eq(0x3A, frames[0].bytes[0]);
        assert_eq(0x3A, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);
        assert_eq(0, frames[0].is_special);
    }

    /* Caps Lock repeat produces 0 frames */
    rc = usb_hid_keyboard_update(&kbd, r_caps, 8, frames);
    assert_eq(0, rc);

    /* Caps Lock release produces 0xBA break */
    const uint8_t r_none[8] = { 0 };
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(1, frames[0].length);
        assert_eq(0xBA, frames[0].bytes[0]);
        assert_eq(0x3A, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);
        assert_eq(0, frames[0].is_special);
    }

    /* 2. PrintScreen (0x46) */
    const uint8_t r_prtscn[8] = { 0x00, 0x00, 0x46, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_prtscn, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(4, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0x2A, frames[0].bytes[1]);
        assert_eq(0xE0, frames[0].bytes[2]);
        assert_eq(0x37, frames[0].bytes[3]);
        assert_eq(0, frames[0].physical_key); /* Special frame physical_key = 0 */
        assert_eq(1, frames[0].is_make);
        assert_eq(1, frames[0].is_special);
    }

    /* PrintScreen repeat produces 0 frames */
    rc = usb_hid_keyboard_update(&kbd, r_prtscn, 8, frames);
    assert_eq(0, rc);

    /* PrintScreen release produces 4-byte break sequence: E0 B7 E0 AA */
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(4, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0xB7, frames[0].bytes[1]);
        assert_eq(0xE0, frames[0].bytes[2]);
        assert_eq(0xAA, frames[0].bytes[3]);
        assert_eq(0, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);
        assert_eq(1, frames[0].is_special);
    }

    /* PrintScreen release via usb_hid_keyboard_release */
    rc = usb_hid_keyboard_update(&kbd, r_prtscn, 8, frames);
    assert_eq(1, rc);
    count = usb_hid_keyboard_release(&kbd, frames);
    assert_eq(1, count);
    if (count == 1) {
        assert_eq(4, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0xB7, frames[0].bytes[1]);
        assert_eq(0xE0, frames[0].bytes[2]);
        assert_eq(0xAA, frames[0].bytes[3]);
        assert_eq(0, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);
        assert_eq(1, frames[0].is_special);
    }

    /* 3. Pause (0x48): emits E1 make once, no break on release */
    const uint8_t r_pause[8] = { 0x00, 0x00, 0x48, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_pause, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(6, frames[0].length);
        assert_eq(0xE1, frames[0].bytes[0]);
        assert_eq(0x1D, frames[0].bytes[1]);
        assert_eq(0x45, frames[0].bytes[2]);
        assert_eq(0xE1, frames[0].bytes[3]);
        assert_eq(0x9D, frames[0].bytes[4]);
        assert_eq(0xC5, frames[0].bytes[5]);
        assert_eq(0, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);
        assert_eq(1, frames[0].is_special);
    }

    /* Pause repeat produces 0 frames */
    rc = usb_hid_keyboard_update(&kbd, r_pause, 8, frames);
    assert_eq(0, rc);

    /* Pause release produces 0 frames (no break in PS/2 Set 1) */
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(0, rc);

    /* Pause release via usb_hid_keyboard_release produces 0 frames */
    rc = usb_hid_keyboard_update(&kbd, r_pause, 8, frames);
    assert_eq(1, rc);
    count = usb_hid_keyboard_release(&kbd, frames);
    assert_eq(0, count);
}

static void test_keyboard_mapping_coverage(void)
{
    usb_hid_keyboard_t kbd;
    input_ps2_frame_t frames[USB_HID_MAX_FRAMES];
    const uint8_t r_none[8] = { 0 };
    int rc;

    memset(&kbd, 0, sizeof(kbd));

    /* 1. Right Alt: modifier bit 6 (0x40) -> E0 38 */
    const uint8_t r_ralt[8] = { 0x40, 0, 0, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_ralt, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(2, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0x38, frames[0].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x38, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);
    }
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(2, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0xB8, frames[0].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x38, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);
    }

    /* 2. Arrow keys:
     * Up (0x52) -> E0 48
     * Down (0x51) -> E0 50
     * Left (0x50) -> E0 4B
     * Right (0x4F) -> E0 4D */
    const uint8_t r_arrows[8] = { 0x00, 0, 0x4F, 0x50, 0x51, 0x52, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_arrows, 8, frames);
    assert_eq(4, rc);
    if (rc == 4) {
        /* Frames are ordered by ascending usage: 0x4F, 0x50, 0x51, 0x52 */
        /* Right arrow (0x4F) */
        assert_eq(2, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0x4D, frames[0].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x4D, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);

        /* Left arrow (0x50) */
        assert_eq(2, frames[1].length);
        assert_eq(0xE0, frames[1].bytes[0]);
        assert_eq(0x4B, frames[1].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x4B, frames[1].physical_key);
        assert_eq(1, frames[1].is_make);

        /* Down arrow (0x51) */
        assert_eq(2, frames[2].length);
        assert_eq(0xE0, frames[2].bytes[0]);
        assert_eq(0x50, frames[2].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x50, frames[2].physical_key);
        assert_eq(1, frames[2].is_make);

        /* Up arrow (0x52) */
        assert_eq(2, frames[3].length);
        assert_eq(0xE0, frames[3].bytes[0]);
        assert_eq(0x48, frames[3].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x48, frames[3].physical_key);
        assert_eq(1, frames[3].is_make);
    }
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(4, rc);
    if (rc == 4) {
        assert_eq(0xCD, frames[0].bytes[1]);
        assert_eq(0xCB, frames[1].bytes[1]);
        assert_eq(0xD0, frames[2].bytes[1]);
        assert_eq(0xC8, frames[3].bytes[1]);
    }

    /* 3. Keypad Enter (0x58) -> E0 1C */
    const uint8_t r_kpenter[8] = { 0, 0, 0x58, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_kpenter, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(2, frames[0].length);
        assert_eq(0xE0, frames[0].bytes[0]);
        assert_eq(0x1C, frames[0].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x1C, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);
    }
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(0x9C, frames[0].bytes[1]);
    }

    /* 4. F12 (0x45) -> 0x58 */
    const uint8_t r_f12[8] = { 0, 0, 0x45, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r_f12, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(1, frames[0].length);
        assert_eq(0x58, frames[0].bytes[0]);
        assert_eq(0x58, frames[0].physical_key);
        assert_eq(1, frames[0].is_make);
    }
    rc = usb_hid_keyboard_update(&kbd, r_none, 8, frames);
    assert_eq(1, rc);
    if (rc == 1) {
        assert_eq(0xD8, frames[0].bytes[0]);
    }
}

static void test_keyboard_ordering(void)
{
    usb_hid_keyboard_t kbd;
    input_ps2_frame_t frames[USB_HID_MAX_FRAMES];
    int rc;

    memset(&kbd, 0, sizeof(kbd));

    /* Initial state: Left Shift (0x02) held, 'b' (0x05) held */
    const uint8_t r1[8] = { 0x02, 0, 0x05, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r1, 8, frames);
    assert_eq(2, rc);

    /* Transition: Right Alt (0x40) held, 'a' (0x04) held.
     * Four-group order:
     * 1. Ordinary break: 'b' (0x05) -> 0xB0
     * 2. Modifier break: Left Shift (0x02) -> 0xAA
     * 3. Modifier make: Right Alt (0x40) -> E0 38
     * 4. Ordinary make: 'a' (0x04) -> 0x1E
     */
    const uint8_t r2[8] = { 0x40, 0, 0x04, 0, 0, 0, 0, 0 };
    rc = usb_hid_keyboard_update(&kbd, r2, 8, frames);
    assert_eq(4, rc);
    if (rc == 4) {
        /* Group 1: 'b' break */
        assert_eq(0xB0, frames[0].bytes[0]);
        assert_eq(0x30, frames[0].physical_key);
        assert_eq(0, frames[0].is_make);

        /* Group 2: Left Shift break */
        assert_eq(0xAA, frames[1].bytes[0]);
        assert_eq(0x2A, frames[1].physical_key);
        assert_eq(0, frames[1].is_make);

        /* Group 3: Right Alt make */
        assert_eq(0xE0, frames[2].bytes[0]);
        assert_eq(0x38, frames[2].bytes[1]);
        assert_eq(INPUT_PS2_KEY_E0 | 0x38, frames[2].physical_key);
        assert_eq(1, frames[2].is_make);

        /* Group 4: 'a' make */
        assert_eq(0x1E, frames[3].bytes[0]);
        assert_eq(0x1E, frames[3].physical_key);
        assert_eq(1, frames[3].is_make);
    }
}

static void test_mouse_boot_prefix(void)
{
    mouse_event_t out;
    int rc;

    /* 1. Valid 4-byte report with MPS=4:
     * {5, 0xfe, 0x7f, 0x55} -> buttons=5, dx=-2, dy=127, wheel=0, reserved=0 */
    const uint8_t r_valid[4] = { 5, 0xfe, 0x7f, 0x55 };
    memset(&out, 0x5a, sizeof(out));
    rc = usb_hid_mouse_decode(r_valid, 4, 4, &out);
    assert_eq(0, rc);
    assert_eq(5, out.buttons);
    assert_eq(-2, out.dx);
    assert_eq(127, out.dy);
    assert_eq(0, out.wheel);
    assert_eq(0, out.reserved);

    /* 2. Length 2 rejected, out untouched */
    memset(&out, 0x5a, sizeof(out));
    rc = usb_hid_mouse_decode(r_valid, 2, 4, &out);
    assert_eq(-EINVAL, rc);
    assert_eq(0x5a, (uint8_t)out.buttons);

    /* 3. Length > MPS rejected, out untouched */
    memset(&out, 0x5a, sizeof(out));
    rc = usb_hid_mouse_decode(r_valid, 4, 3, &out);
    assert_eq(-EINVAL, rc);
    assert_eq(0x5a, (uint8_t)out.buttons);

    /* 4. MPS < 3 rejected */
    assert_eq(-EINVAL, usb_hid_mouse_decode(r_valid, 3, 2, &out));

    /* 5. NULL pointer rejected */
    assert_eq(-EINVAL, usb_hid_mouse_decode(NULL, 4, 4, &out));
    assert_eq(-EINVAL, usb_hid_mouse_decode(r_valid, 4, 4, NULL));

    /* 6. Minimal 3-byte report with buttons mask */
    const uint8_t r3[3] = { 0xF9, 0x10, 0xF0 }; /* buttons & 0x07 = 1 */
    rc = usb_hid_mouse_decode(r3, 3, 8, &out);
    assert_eq(0, rc);
    assert_eq(1, out.buttons);
    assert_eq(16, out.dx);
    assert_eq(-16, out.dy);
    assert_eq(0, out.wheel);
    assert_eq(0, out.reserved);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_keyboard_boot_diff),
    TEST_ENTRY(test_keyboard_release_and_held_inputs),
    TEST_ENTRY(test_caps_printscreen_pause),
    TEST_ENTRY(test_keyboard_mapping_coverage),
    TEST_ENTRY(test_keyboard_ordering),
    TEST_ENTRY(test_mouse_boot_prefix),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("USB Boot HID Report Decoder");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
