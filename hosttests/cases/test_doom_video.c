/*
 * test_doom_video.c — Doom platform video presentation host tests (T1)
 *
 * Verifies:
 *   - center_1024_768: 1024x768 yields scale=3, 960x600 centered at (32, 84)
 *   - nearest_neighbor_1_2_3: 1x, 2x, 3x integer scaling pixel-for-pixel match
 *   - full_present_retry: initial full present retries on EAGAIN before rect present
 *   - fatal_present_cleanup: permanent present error does not re-acquire and fails
 *   - overflow_and_undersized: screen < 320x200 or invalid scale rejected without acquire
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
#include "doom_video.h"

#define FAKE_FB_FD 555
#define DUMMY_HANDLE ((gfx_handle_t *)(uintptr_t)0x12340000)

/* Mock state */
static struct fb_info g_mock_fb_info;
static bool g_mock_fb_open_fail;
static int g_mock_fb_open_errno;
static bool g_mock_fb_read_fail;
static int g_mock_fb_read_errno;
static bool g_mock_fb_short_read;
static int g_mock_fb_close_count;

static int g_mock_gfx_open_count;
static bool g_mock_gfx_open_fail;
static int g_mock_gfx_open_errno;
static int g_mock_gfx_close_count;

static int g_mock_acquire_count;
static int g_mock_acquire_errno;
static int g_mock_release_count;

static int g_mock_fill_rect_count;

static int g_mock_sprite_blit_count;
static int32_t g_mock_last_blit_dx;
static int32_t g_mock_last_blit_dy;
static uint32_t g_mock_last_blit_w;
static uint32_t g_mock_last_blit_h;
static uint32_t g_mock_captured_pixels[960 * 600];

static int g_mock_present_count;
static int g_mock_present_errno;
static int g_mock_present_rect_count;
static int g_mock_present_rect_errno;
static uint32_t g_mock_last_rect_x;
static uint32_t g_mock_last_rect_y;
static uint32_t g_mock_last_rect_w;
static uint32_t g_mock_last_rect_h;

static void reset_mocks(void)
{
    memset(&g_mock_fb_info, 0, sizeof(g_mock_fb_info));
    g_mock_fb_info.width = 1024;
    g_mock_fb_info.height = 768;
    g_mock_fb_info.stride = 1024 * 4;
    g_mock_fb_info.bpp = 32;
    g_mock_fb_info.format = FB_FORMAT_RGB32;

    g_mock_fb_open_fail = false;
    g_mock_fb_open_errno = 0;
    g_mock_fb_read_fail = false;
    g_mock_fb_read_errno = 0;
    g_mock_fb_short_read = false;
    g_mock_fb_close_count = 0;

    g_mock_gfx_open_count = 0;
    g_mock_gfx_open_fail = false;
    g_mock_gfx_open_errno = 0;
    g_mock_gfx_close_count = 0;

    g_mock_acquire_count = 0;
    g_mock_acquire_errno = 0;
    g_mock_release_count = 0;

    g_mock_fill_rect_count = 0;

    g_mock_sprite_blit_count = 0;
    g_mock_last_blit_dx = 0;
    g_mock_last_blit_dy = 0;
    g_mock_last_blit_w = 0;
    g_mock_last_blit_h = 0;
    memset(g_mock_captured_pixels, 0, sizeof(g_mock_captured_pixels));

    g_mock_present_count = 0;
    g_mock_present_errno = 0;
    g_mock_present_rect_count = 0;
    g_mock_present_rect_errno = 0;
    g_mock_last_rect_x = 0;
    g_mock_last_rect_y = 0;
    g_mock_last_rect_w = 0;
    g_mock_last_rect_h = 0;
}

/* Wrapped libc functions */
extern int __real_open(const char *path, int flags, ...);
extern ssize_t __real_read(int fd, void *buf, size_t count);
extern int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...)
{
    if (path && strcmp(path, "/dev/fb") == 0) {
        if (g_mock_fb_open_fail) {
            errno = g_mock_fb_open_errno ? g_mock_fb_open_errno : ENODEV;
            return -1;
        }
        return FAKE_FB_FD;
    }
    return -1;
}

ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    if (fd == FAKE_FB_FD) {
        if (g_mock_fb_read_fail) {
            errno = g_mock_fb_read_errno ? g_mock_fb_read_errno : EIO;
            return -1;
        }
        if (g_mock_fb_short_read) {
            if (count > sizeof(g_mock_fb_info) / 2) {
                count = sizeof(g_mock_fb_info) / 2;
            }
        } else {
            if (count > sizeof(g_mock_fb_info)) {
                count = sizeof(g_mock_fb_info);
            }
        }
        memcpy(buf, &g_mock_fb_info, count);
        return (ssize_t)count;
    }
    return -1;
}

int __wrap_close(int fd)
{
    if (fd == FAKE_FB_FD) {
        g_mock_fb_close_count++;
        return 0;
    }
    return 0;
}

/* Mock libgfx implementations */
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    g_mock_gfx_open_count++;
    if (g_mock_gfx_open_fail) {
        errno = g_mock_gfx_open_errno ? g_mock_gfx_open_errno : ENODEV;
        return NULL;
    }
    return DUMMY_HANDLE;
}

void gfx_close(gfx_handle_t *h)
{
    if (h) {
        g_mock_gfx_close_count++;
    }
}

int gfx_acquire(gfx_handle_t *h)
{
    g_mock_acquire_count++;
    if (g_mock_acquire_errno) {
        errno = g_mock_acquire_errno;
        return -1;
    }
    return 0;
}

int gfx_release(gfx_handle_t *h)
{
    g_mock_release_count++;
    return 0;
}

void gfx__fill_rect_impl(gfx_handle_t *h, int32_t x, int32_t y,
                         uint32_t w, uint32_t h_, uint32_t color)
{
    (void)h; (void)x; (void)y; (void)w; (void)h_; (void)color;
    g_mock_fill_rect_count++;
}

void gfx_sprite_blit(gfx_handle_t *h, int32_t dx, int32_t dy,
                     const uint32_t *src, uint32_t src_stride,
                     uint32_t src_w, uint32_t src_h,
                     bool use_color_key, uint32_t color_key)
{
    (void)h; (void)use_color_key; (void)color_key;
    g_mock_sprite_blit_count++;
    g_mock_last_blit_dx = dx;
    g_mock_last_blit_dy = dy;
    g_mock_last_blit_w = src_w;
    g_mock_last_blit_h = src_h;

    size_t copy_pixels = (size_t)src_w * src_h;
    if (copy_pixels > sizeof(g_mock_captured_pixels) / sizeof(uint32_t)) {
        copy_pixels = sizeof(g_mock_captured_pixels) / sizeof(uint32_t);
    }
    memcpy(g_mock_captured_pixels, src, copy_pixels * sizeof(uint32_t));
}

int gfx_present(gfx_handle_t *h)
{
    (void)h;
    g_mock_present_count++;
    if (g_mock_present_errno) {
        errno = g_mock_present_errno;
        return -1;
    }
    return 0;
}

int gfx_present_rect(gfx_handle_t *h, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h_)
{
    (void)h;
    g_mock_present_rect_count++;
    g_mock_last_rect_x = x;
    g_mock_last_rect_y = y;
    g_mock_last_rect_w = w;
    g_mock_last_rect_h = h_;
    if (g_mock_present_rect_errno) {
        errno = g_mock_present_rect_errno;
        return -1;
    }
    return 0;
}

/* ── Test Cases ─────────────────────────────────────────────── */

static void test_center_1024_768(void)
{
    reset_mocks();
    g_mock_fb_info.width = 1024;
    g_mock_fb_info.height = 768;

    doom_video_t v;
    memset(&v, 0, sizeof(v));

    int rc = doom_video_open(&v, 0); /* 0 = auto scale */
    assert_eq(0, rc);
    assert_eq(1024, v.screen_w);
    assert_eq(768, v.screen_h);
    assert_eq(3, v.scale);
    assert_eq(960, v.scaled_w);
    assert_eq(600, v.scaled_h);
    assert_eq(32, v.offset_x);
    assert_eq(84, v.offset_y);
    assert_true(v.handle != NULL);
    assert_eq(DUMMY_HANDLE, doom_video_handle(&v));

    uint32_t dummy_pixels[320 * 200];
    memset(dummy_pixels, 0x11, sizeof(dummy_pixels));
    rc = doom_video_draw(&v, dummy_pixels);
    assert_eq(0, rc);
    assert_eq(1, g_mock_fb_close_count);
    assert_eq(1, g_mock_fill_rect_count);
    assert_eq(1, g_mock_sprite_blit_count);
    assert_eq(1, g_mock_present_count);
    assert_eq(32, g_mock_last_blit_dx);
    assert_eq(84, g_mock_last_blit_dy);
    assert_eq(960, g_mock_last_blit_w);
    assert_eq(600, g_mock_last_blit_h);

    doom_video_close(&v);
    assert_eq(1, g_mock_release_count);
    assert_eq(1, g_mock_gfx_close_count);
    assert_true(doom_video_handle(&v) == NULL);
}

static void test_nearest_neighbor_1_2_3(void)
{
    uint32_t src[320 * 200];
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 320; x++) {
            src[y * 320 + x] = ((uint32_t)y << 16) | (uint32_t)x | 0xFF000000u;
        }
    }

    for (unsigned s = 1; s <= 3; s++) {
        reset_mocks();
        g_mock_fb_info.width = 1024;
        g_mock_fb_info.height = 768;

        doom_video_t v;
        memset(&v, 0, sizeof(v));

        int rc = doom_video_open(&v, s);
        assert_eq(0, rc);
        assert_eq(s, v.scale);
        assert_eq(320 * s, v.scaled_w);
        assert_eq(200 * s, v.scaled_h);

        rc = doom_video_draw(&v, src);
        assert_eq(0, rc);
        assert_eq(320 * s, g_mock_last_blit_w);
        assert_eq(200 * s, g_mock_last_blit_h);

        /* Pixel-by-pixel match verification */
        bool match = true;
        for (uint32_t sy = 0; sy < 200 * s; sy++) {
            for (uint32_t sx = 0; sx < 320 * s; sx++) {
                uint32_t expected = src[(sy / s) * 320 + (sx / s)];
                uint32_t actual = g_mock_captured_pixels[sy * (320 * s) + sx];
                if (expected != actual) {
                    match = false;
                    printf("Mismatch at scale=%u (%u, %u): expected 0x%08x, got 0x%08x\n",
                           s, sx, sy, expected, actual);
                    break;
                }
            }
            if (!match) break;
        }
        assert_true(match);

        doom_video_close(&v);
    }
}

static void test_full_present_retry(void)
{
    reset_mocks();
    /* Force first present (in open or draw) to return EAGAIN */
    g_mock_present_errno = EAGAIN;

    doom_video_t v;
    memset(&v, 0, sizeof(v));

    int rc = doom_video_open(&v, 0);
    assert_eq(0, rc);

    /* First draw: initial present still pending, should attempt full present */
    uint32_t dummy[320 * 200];
    memset(dummy, 0x22, sizeof(dummy));

    rc = doom_video_draw(&v, dummy);
    /* Returns 1 (EAGAIN, retry) */
    assert_eq(1, rc);
    assert_eq(0, g_mock_present_rect_count); /* must NOT degrade to rect present */

    /* Second draw: present succeeds now */
    g_mock_present_errno = 0;
    rc = doom_video_draw(&v, dummy);
    assert_eq(0, rc);
    assert_eq(0, g_mock_present_rect_count); /* was still full present */

    /* Third draw: now that full present succeeded, normal rect present is used */
    rc = doom_video_draw(&v, dummy);
    assert_eq(0, rc);
    assert_eq(1, g_mock_present_rect_count);
    assert_eq(v.offset_x, g_mock_last_rect_x);
    assert_eq(v.offset_y, g_mock_last_rect_y);
    assert_eq(v.scaled_w, g_mock_last_rect_w);
    assert_eq(v.scaled_h, g_mock_last_rect_h);

    doom_video_close(&v);
}

static void test_fatal_present_cleanup(void)
{
    reset_mocks();
    doom_video_t v;
    memset(&v, 0, sizeof(v));

    int rc = doom_video_open(&v, 0);
    assert_eq(0, rc);
    int initial_acquires = g_mock_acquire_count;

    /* First draw succeeds (full present) */
    uint32_t dummy[320 * 200];
    rc = doom_video_draw(&v, dummy);
    assert_eq(0, rc);

    /* Next draw hits fatal error (EACCES) */
    g_mock_present_rect_errno = EACCES;
    rc = doom_video_draw(&v, dummy);
    assert_eq(-1, rc);
    assert_eq(EACCES, errno);
    /* Crucial check: must NOT attempt to re-acquire on fatal error! */
    assert_eq(initial_acquires, g_mock_acquire_count);

    doom_video_close(&v);
    assert_true(doom_video_handle(&v) == NULL);
}

static void test_overflow_and_undersized(void)
{
    /* Undersized screen: 300x200 */
    reset_mocks();
    g_mock_fb_info.width = 300;
    g_mock_fb_info.height = 200;
    doom_video_t v;
    memset(&v, 0, sizeof(v));
    int rc = doom_video_open(&v, 0);
    assert_eq(-1, rc);
    assert_eq(0, g_mock_acquire_count);
    assert_eq(0, g_mock_gfx_open_count);

    /* Scale too large to fit: 640x480 with requested scale 3 (needs 960x600) */
    reset_mocks();
    g_mock_fb_info.width = 640;
    g_mock_fb_info.height = 480;
    rc = doom_video_open(&v, 3);
    assert_eq(-1, rc);
    assert_eq(0, g_mock_acquire_count);

    /* Invalid scale requested: 4 */
    reset_mocks();
    g_mock_fb_info.width = 1920;
    g_mock_fb_info.height = 1080;
    rc = doom_video_open(&v, 4);
    assert_eq(-1, rc);
    assert_eq(0, g_mock_acquire_count);

    /* /dev/fb read failure */
    reset_mocks();
    g_mock_fb_read_fail = true;
    rc = doom_video_open(&v, 0);
    assert_eq(-1, rc);
    assert_eq(0, g_mock_acquire_count);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_center_1024_768),
    TEST_ENTRY(test_nearest_neighbor_1_2_3),
    TEST_ENTRY(test_full_present_retry),
    TEST_ENTRY(test_fatal_present_cleanup),
    TEST_ENTRY(test_overflow_and_undersized),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
