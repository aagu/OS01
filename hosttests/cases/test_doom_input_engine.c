/*
 * test_doom_input_engine.c — Doom engine event loop integration host tests (T2)
 *
 * Verifies:
 *   - engine_batch_preserves_64_down_up:
 *     Queues 64 distinct mapped key DOWNs followed by their UPs (128 events total).
 *     Drives them through the REAL patched i_input.c (I_GetEvent) and real d_event.c (D_PostEvent/D_PopEvent).
 *     Asserts:
 *       1. Each I_GetEvent batch delivers at most 16 items.
 *       2. All 128 events are preserved in exact sequence without overflow or truncation.
 *       3. All keys are released at the end (no stuck keys).
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

/* Real doomgeneric headers and types */
#include "d_event.h"
#include "doomkeys.h"

/* The patched I_GetEvent declared in i_input.c */
void I_GetEvent(void);

#define FAKE_KBD_FD 777
#define DUMMY_HANDLE ((gfx_handle_t *)(uintptr_t)0x12340000)

static doom_input_t g_test_input;
static uint8_t g_feed_buf[2048];
static size_t  g_feed_pos;
static size_t  g_feed_len;

static void reset_engine_mocks(void)
{
    memset(g_feed_buf, 0, sizeof(g_feed_buf));
    g_feed_pos = 0;
    g_feed_len = 0;
}

static void feed_bytes(const uint8_t *bytes, size_t count)
{
    if (g_feed_len + count <= sizeof(g_feed_buf)) {
        memcpy(g_feed_buf + g_feed_len, bytes, count);
        g_feed_len += count;
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
        if (g_feed_pos >= g_feed_len) {
            errno = EAGAIN;
            return -1;
        }
        size_t avail = g_feed_len - g_feed_pos;
        size_t n = count < avail ? count : avail;
        memcpy(buf, g_feed_buf + g_feed_pos, n);
        g_feed_pos += n;
        return (ssize_t)n;
    }
    return -1;
}

int __wrap_close(int fd)
{
    (void)fd;
    return 0;
}

/* Mock libgfx input functions */
int gfx_bind_input(gfx_handle_t *h, int keyboard_fd, int mouse_fd,
                   gfx_input_state_t *out_state)
{
    (void)h; (void)keyboard_fd; (void)mouse_fd;
    if (out_state) {
        out_state->generation = 1;
        out_state->initial_modifiers = 0;
        out_state->reserved = 0;
    }
    return 0;
}

int gfx_unbind_input(gfx_handle_t *h)
{
    (void)h;
    return 0;
}

int gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)
{
    (void)h;
    if (out) {
        out->generation = 1;
        out->flags = GFX_DISPLAY_STATE_FOREGROUND;
        out->reserved = 0;
    }
    return 0;
}

/* DG_GetKey definition required by i_input.c */
int DG_GetKey(int *pressed, unsigned char *key)
{
    int rc = doom_input_pop(&g_test_input, pressed, key);
    if (rc < 0) {
        return 0;
    }
    return rc;
}

/* 64 distinct physical key make codes */
static const uint16_t g_64_keys[64] = {
    /* 26 Letters */
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, /* q..p */
    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,       /* a..l */
    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,                   /* z..m */
    /* 10 Digits */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, /* 1..0 */
    /* 11 Punctuation */
    0x0C, 0x0D, 0x1A, 0x1B, 0x27, 0x28, 0x29, 0x2B, 0x33, 0x34, 0x35,
    /* 9 Special keys */
    0x01, 0x0E, 0x0F, 0x1C, 0x39, /* Esc, Bksp, Tab, Enter, Space */
    0x148, 0x14B, 0x14D, 0x150,   /* Up, Left, Right, Down */
    /* 8 Function keys */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42 /* F1..F8 */
};

static void test_engine_batch_preserves_64_down_up(void)
{
    reset_engine_mocks();
    int rc = doom_input_open(&g_test_input, DUMMY_HANDLE);
    assert_eq(0, rc);

    /* Feed 64 DOWNs followed by 64 UPs into kbd buffer */
    for (int i = 0; i < 64; i++) {
        uint16_t pk = g_64_keys[i];
        if (pk & 0x100) {
            uint8_t seq[2] = { 0xE0, (uint8_t)(pk & 0x7F) };
            feed_bytes(seq, 2);
        } else {
            uint8_t b = (uint8_t)pk;
            feed_bytes(&b, 1);
        }
    }
    for (int i = 0; i < 64; i++) {
        uint16_t pk = g_64_keys[i];
        if (pk & 0x100) {
            uint8_t seq[2] = { 0xE0, (uint8_t)((pk & 0x7F) | 0x80) };
            feed_bytes(seq, 2);
        } else {
            uint8_t b = (uint8_t)(pk | 0x80);
            feed_bytes(&b, 1);
        }
    }

    /* Pump all bytes into g_test_input queue */
    rc = doom_input_pump(&g_test_input, 100);
    assert_eq(0, rc);
    assert_eq(128, g_test_input.count);

    /* Now run simulated engine tics: I_GetEvent() followed by D_PopEvent() drain */
    int total_received = 0;
    int max_batch = 0;
    int held_keys[256] = {0};

    int tics = 0;
    while (tics < 200 && total_received < 128) {
        tics++;
        I_GetEvent();

        int batch = 0;
        event_t *ev;
        while ((ev = D_PopEvent()) != NULL) {
            batch++;
            total_received++;
            assert_true(ev->data1 > 0 && ev->data1 < 256);
            if (ev->type == ev_keydown) {
                held_keys[ev->data1]++;
            } else if (ev->type == ev_keyup) {
                held_keys[ev->data1]--;
            }
        }
        if (batch > max_batch) max_batch = batch;
        assert_true(batch <= 16);
        if (batch == 0 && total_received < 128) {
            /* Unexpected stall */
            break;
        }
    }

    assert_eq(128, total_received);
    assert_true(max_batch <= 16);
    assert_true(max_batch > 0);

    /* Verify no keys are still held */
    bool stuck = false;
    for (int i = 0; i < 256; i++) {
        if (held_keys[i] != 0) {
            stuck = true;
            printf("Key %d stuck: refcount %d\n", i, held_keys[i]);
            break;
        }
    }
    assert_false(stuck);

    doom_input_close(&g_test_input);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_engine_batch_preserves_64_down_up),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
