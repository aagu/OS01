/*
 * test_doom_input.c — Doom platform PS2 input adapter host tests (T2)
 *
 * Verifies:
 *   - two_ctrl_one_release: press LCtrl then RCtrl yields 1 DOWN; release LCtrl yields no UP; release RCtrl yields 1 UP
 *   - e0_release_identity: extended key (Up arrow E0 48 / E0 C8) produces identical Doom key on DOWN and UP
 *   - ring_capacity_256: pushing 257th event fails without wrapping/overwriting, clears queue and latches error
 *   - lease_error_clears_pending: read error (EOVERFLOW/EACCES/ESTALE) terminates pump, clears pending events, pop returns -1
 *   - owner_revoked_between_pump_pop: revoking display owner after pump causes pop to check owner, clear queue and return -1
 *   - typematic_ignored: duplicate make codes do not generate extra events
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_framework.h"
#include <uapi/fb.h>
#include <gfx.h>
#include "doom_input.h"

#define FAKE_KBD_FD 777
#define DUMMY_HANDLE ((gfx_handle_t *)(uintptr_t)0x12340000)

#define KEY_FIRE       0xa3
#define KEY_USE        0xa2
#define KEY_RSHIFT     (0x80+0x36)
#define KEY_LALT       (0x80+0x38)
#define KEY_UPARROW    0xad
#define KEY_DOWNARROW  0xaf
#define KEY_LEFTARROW  0xac
#define KEY_RIGHTARROW 0xae
#define KEY_ENTER      13
#define KEY_ESCAPE     27
#define KEY_TAB        9
#define KEY_BACKSPACE  127

/* Mock state */
static uint8_t g_mock_kbd_buf[1024];
static size_t  g_mock_kbd_pos;
static size_t  g_mock_kbd_len;
static int     g_mock_kbd_read_err;
static int     g_mock_kbd_close_count;

static int      g_mock_bind_count;
static int      g_mock_bind_err;
static uint32_t g_mock_initial_modifiers;
static int      g_mock_unbind_count;

static uint32_t g_mock_display_flags;
static int      g_mock_display_state_err;

static void reset_mocks(void)
{
    memset(g_mock_kbd_buf, 0, sizeof(g_mock_kbd_buf));
    g_mock_kbd_pos = 0;
    g_mock_kbd_len = 0;
    g_mock_kbd_read_err = 0;
    g_mock_kbd_close_count = 0;

    g_mock_bind_count = 0;
    g_mock_bind_err = 0;
    g_mock_initial_modifiers = 0;
    g_mock_unbind_count = 0;

    g_mock_display_flags = GFX_DISPLAY_STATE_FOREGROUND;
    g_mock_display_state_err = 0;
}

static void feed_bytes(const uint8_t *bytes, size_t count)
{
    if (g_mock_kbd_len + count <= sizeof(g_mock_kbd_buf)) {
        memcpy(g_mock_kbd_buf + g_mock_kbd_len, bytes, count);
        g_mock_kbd_len += count;
    }
}

/* Wrapped libc functions */
int __wrap_open(const char *path, int flags, ...)
{
    if (path && strcmp(path, "/dev/keyboard") == 0) {
        return FAKE_KBD_FD;
    }
    return -1;
}

ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    if (fd == FAKE_KBD_FD) {
        if (g_mock_kbd_read_err) {
            errno = g_mock_kbd_read_err;
            return -1;
        }
        if (g_mock_kbd_pos >= g_mock_kbd_len) {
            errno = EAGAIN;
            return -1;
        }
        size_t avail = g_mock_kbd_len - g_mock_kbd_pos;
        size_t n = count < avail ? count : avail;
        memcpy(buf, g_mock_kbd_buf + g_mock_kbd_pos, n);
        g_mock_kbd_pos += n;
        return (ssize_t)n;
    }
    return -1;
}

int __wrap_close(int fd)
{
    if (fd == FAKE_KBD_FD) {
        g_mock_kbd_close_count++;
        return 0;
    }
    return 0;
}

/* Mock libgfx input functions */
int gfx_bind_input(gfx_handle_t *h, int keyboard_fd, int mouse_fd,
                   gfx_input_state_t *out_state)
{
    (void)h; (void)keyboard_fd; (void)mouse_fd;
    g_mock_bind_count++;
    if (g_mock_bind_err) {
        errno = g_mock_bind_err;
        return -1;
    }
    if (out_state) {
        out_state->generation = 1;
        out_state->initial_modifiers = g_mock_initial_modifiers;
        out_state->reserved = 0;
    }
    return 0;
}

int gfx_unbind_input(gfx_handle_t *h)
{
    (void)h;
    g_mock_unbind_count++;
    return 0;
}

int gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)
{
    (void)h;
    if (g_mock_display_state_err) {
        errno = g_mock_display_state_err;
        return -1;
    }
    if (out) {
        out->generation = 1;
        out->flags = g_mock_display_flags;
        out->reserved = 0;
    }
    return 0;
}

/* ── Test Cases ─────────────────────────────────────────────── */

static void test_two_ctrl_one_release(void)
{
    reset_mocks();
    doom_input_t in;
    int rc = doom_input_open(&in, DUMMY_HANDLE);
    assert_eq(0, rc);
    assert_eq(1, g_mock_bind_count);

    /* 1. Left Ctrl press (0x1D) */
    uint8_t lctrl_make[] = { 0x1D };
    feed_bytes(lctrl_make, sizeof(lctrl_make));
    rc = doom_input_pump(&in, 10);
    assert_eq(0, rc);

    int pressed = -1;
    unsigned char key = 0;
    assert_eq(1, doom_input_pop(&in, &pressed, &key));
    assert_eq(1, pressed);
    assert_eq(KEY_FIRE, (int)key);

    /* Queue should be empty now */
    assert_eq(0, doom_input_pop(&in, &pressed, &key));

    /* 2. Right Ctrl press (0xE0, 0x1D) */
    uint8_t rctrl_make[] = { 0xE0, 0x1D };
    feed_bytes(rctrl_make, sizeof(rctrl_make));
    rc = doom_input_pump(&in, 20);
    assert_eq(0, rc);

    /* Should NOT emit duplicate DOWN */
    assert_eq(0, doom_input_pop(&in, &pressed, &key));

    /* 3. Left Ctrl release (0x9D) */
    uint8_t lctrl_break[] = { 0x9D };
    feed_bytes(lctrl_break, sizeof(lctrl_break));
    rc = doom_input_pump(&in, 30);
    assert_eq(0, rc);

    /* Should NOT emit UP yet because Right Ctrl is still held */
    assert_eq(0, doom_input_pop(&in, &pressed, &key));

    /* 4. Right Ctrl release (0xE0, 0x9D) */
    uint8_t rctrl_break[] = { 0xE0, 0x9D };
    feed_bytes(rctrl_break, sizeof(rctrl_break));
    rc = doom_input_pump(&in, 40);
    assert_eq(0, rc);

    /* NOW it must emit exactly one UP */
    assert_eq(1, doom_input_pop(&in, &pressed, &key));
    assert_eq(0, pressed);
    assert_eq(KEY_FIRE, (int)key);
    assert_eq(0, doom_input_pop(&in, &pressed, &key));

    doom_input_close(&in);
    assert_eq(1, g_mock_unbind_count);
    assert_eq(1, g_mock_kbd_close_count);
}

static void test_e0_release_identity(void)
{
    reset_mocks();
    doom_input_t in;
    assert_eq(0, doom_input_open(&in, DUMMY_HANDLE));

    /* Up arrow press: E0 48 */
    uint8_t up_make[] = { 0xE0, 0x48 };
    feed_bytes(up_make, sizeof(up_make));
    assert_eq(0, doom_input_pump(&in, 10));

    int pressed = -1;
    unsigned char key = 0;
    assert_eq(1, doom_input_pop(&in, &pressed, &key));
    assert_eq(1, pressed);
    assert_eq(KEY_UPARROW, (int)key);

    /* Up arrow release: E0 C8 */
    uint8_t up_break[] = { 0xE0, 0xC8 };
    feed_bytes(up_break, sizeof(up_break));
    assert_eq(0, doom_input_pump(&in, 20));

    assert_eq(1, doom_input_pop(&in, &pressed, &key));
    assert_eq(0, pressed);
    assert_eq(KEY_UPARROW, (int)key); /* Exact same key identity on UP */

    doom_input_close(&in);
}

static void test_ring_capacity_256(void)
{
    reset_mocks();
    doom_input_t in;
    assert_eq(0, doom_input_open(&in, DUMMY_HANDLE));

    /* Fill the queue to capacity (256 items).
     * We can toggle a key (make, break) 128 times = 256 events. */
    uint8_t toggle[256];
    for (int i = 0; i < 128; i++) {
        toggle[i * 2]     = 0x1E; /* 'a' make */
        toggle[i * 2 + 1] = 0x9E; /* 'a' break */
    }
    feed_bytes(toggle, 256);
    assert_eq(0, doom_input_pump(&in, 10));
    assert_eq(256, in.count);

    /* Pushing 257th event: should overflow and latch error */
    uint8_t extra[] = { 0x1F }; /* 's' make */
    feed_bytes(extra, sizeof(extra));
    int rc = doom_input_pump(&in, 20);
    assert_eq(-1, rc);

    /* After overflow, queue must be cleared and pop returns -1 */
    int pressed;
    unsigned char key;
    assert_eq(-1, doom_input_pop(&in, &pressed, &key));

    doom_input_close(&in);
}

static void test_lease_error_clears_pending(void)
{
    reset_mocks();
    doom_input_t in;
    assert_eq(0, doom_input_open(&in, DUMMY_HANDLE));

    /* Pump some valid keys into queue */
    uint8_t keys[] = { 0x10, 0x90 }; /* 'q' make, break */
    feed_bytes(keys, sizeof(keys));
    assert_eq(0, doom_input_pump(&in, 10));
    assert_eq(2, in.count);

    /* Now inject EOVERFLOW error on next read */
    g_mock_kbd_read_err = EOVERFLOW;
    int rc = doom_input_pump(&in, 20);
    assert_eq(-1, rc);

    /* Pending events must be purged, pop returns -1 */
    int pressed;
    unsigned char key;
    assert_eq(-1, doom_input_pop(&in, &pressed, &key));

    doom_input_close(&in);
}

static void test_owner_revoked_between_pump_pop(void)
{
    reset_mocks();
    doom_input_t in;
    assert_eq(0, doom_input_open(&in, DUMMY_HANDLE));

    /* Pump valid keys */
    uint8_t keys[] = { 0x1E, 0x9E };
    feed_bytes(keys, sizeof(keys));
    assert_eq(0, doom_input_pump(&in, 10));
    assert_eq(2, in.count);

    /* Revoke foreground owner before pop is called (no new pump/frame) */
    g_mock_display_flags = 0; /* no foreground */

    int pressed;
    unsigned char key;
    /* Pop must check owner, detect loss, clear queue and return -1 */
    int rc = doom_input_pop(&in, &pressed, &key);
    assert_eq(-1, rc);
    assert_eq(0, in.count);

    doom_input_close(&in);
}

static void test_typematic_ignored(void)
{
    reset_mocks();
    doom_input_t in;
    assert_eq(0, doom_input_open(&in, DUMMY_HANDLE));

    /* Make followed by repeated makes */
    uint8_t typematic[] = { 0x1E, 0x1E, 0x1E, 0x1E };
    feed_bytes(typematic, sizeof(typematic));
    assert_eq(0, doom_input_pump(&in, 10));

    int pressed;
    unsigned char key;
    assert_eq(1, doom_input_pop(&in, &pressed, &key));
    assert_eq(1, pressed);
    assert_eq('a', (int)key);

    /* No more events! Typematic was ignored */
    assert_eq(0, doom_input_pop(&in, &pressed, &key));

    doom_input_close(&in);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_two_ctrl_one_release),
    TEST_ENTRY(test_e0_release_identity),
    TEST_ENTRY(test_ring_capacity_256),
    TEST_ENTRY(test_lease_error_clears_pending),
    TEST_ENTRY(test_owner_revoked_between_pump_pop),
    TEST_ENTRY(test_typematic_ignored),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
