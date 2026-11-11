/* user/fb_pixel_test/fb_pixel_test.c — host-visible framebuffer pixel
 * acceptance probe (framebuffer cache-policy fix, round 2).
 *
 * The program is the GUEST side of the host-visible pixel check:
 *
 *   1. it takes the framebuffer console away from the kernel console
 *      (FBIOSURRENDER), so nothing but this program writes pixels;
 *   2. it creates a full-screen gfx view and becomes the display owner,
 *      so no other writer can touch the framebuffer;
 *   3. it presents an OLD frame (A), then a NEW frame (B) whose every
 *      pixel differs from A, and prints a DONE line with the real
 *      geometry;
 *   4. it then STOPS writing and waits, so the host can take a QMP
 *      screendump of the settled frame;
 *   5. it presents a RECTANGLE (C) with an off-origin, irregular size
 *      over the B background, prints RECT_DONE and waits again.
 *
 * The pixel values are a pure function of (frame_id, x, y), so the host
 * can recompute the expected image independently:
 *
 *     chk = ((x >> 4) ^ (y >> 4)) & 1          -- 16x16 block checker
 *     r   = (x * 7  + frame * 13 + chk * 97) & 0xFF
 *     g   = (y * 11 + frame * 29 + chk * 53) & 0xFF
 *     b   = ((x ^ y) * 3 + frame * 41) & 0xFF
 *     pixel = (r << 16) | (g << 8) | b         -- 0x00RRGGBB
 *
 * The checker term makes the pattern asymmetric, so a swapped row or a
 * swapped column cannot hide behind a uniform colour; the frame id is
 * in every channel, so frame A and frame B differ in EVERY pixel.
 *
 * The DONE lines are the host's only synchronisation point; the host
 * must take the screendump while this program is waiting.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <uapi/fb.h>
#include <uapi/gfx.h>

#define FRAME_OLD  1u
#define FRAME_FULL 2u
#define FRAME_RECT 3u

#define RECT_X 17u
#define RECT_Y 23u
#define RECT_W 127u
#define RECT_H 61u

/* How long the guest keeps the frame still after each DONE line. */
#define HOLD_SECONDS 15u

static inline uint32_t pattern_pixel(uint32_t frame, uint32_t x, uint32_t y)
{
    uint32_t chk = (((x >> 4) ^ (y >> 4)) & 1u);
    uint32_t r = (x * 7u  + frame * 13u + chk * 97u) & 0xFFu;
    uint32_t g = (y * 11u + frame * 29u + chk * 53u) & 0xFFu;
    uint32_t b = ((x ^ y) * 3u + frame * 41u) & 0xFFu;
    return (r << 16) | (g << 8) | b;
}

static void fill_full(uint32_t *buf, uint32_t stride, uint32_t w, uint32_t h,
                       uint32_t frame)
{
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = buf + (uint64_t)y * (stride / 4);
        for (uint32_t x = 0; x < w; x++) {
            row[x] = pattern_pixel(frame, x, y);
        }
    }
}

static void fill_rect(uint32_t *buf, uint32_t stride, uint32_t w, uint32_t h,
                      uint32_t x0, uint32_t y0, uint32_t rw, uint32_t rh,
                      uint32_t frame)
{
    for (uint32_t y = y0; y < y0 + rh && y < h; y++) {
        uint32_t *row = buf + (uint64_t)y * (stride / 4);
        for (uint32_t x = x0; x < x0 + rw && x < w; x++) {
            row[x] = pattern_pixel(frame, x, y);
        }
    }
}

static int fail(const char *what, int err)
{
    printf("PIXEL_TEST FAIL: %s (errno=%d)\n", what, err);
    fflush(stdout);
    return 1;
}

int main(void)
{
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) return fail("open /dev/fb", errno);

    struct fb_info fbinfo;
    memset(&fbinfo, 0, sizeof(fbinfo));
    if (ioctl(fb_fd, FBIOGET_CURR_MODE, &fbinfo) < 0) {
        return fail("FBIOGET_CURR_MODE", errno);
    }
    if (fbinfo.width == 0 || fbinfo.height == 0 || fbinfo.bpp != 32) {
        printf("PIXEL_TEST FAIL: unexpected fb geometry %ux%u bpp=%u\n",
               fbinfo.width, fbinfo.height, fbinfo.bpp);
        fflush(stdout);
        return 1;
    }

    /* Take the framebuffer console away: from here on the kernel
     * console must not draw over the pixel pattern. */
    if (ioctl(fb_fd, FBIOSURRENDER, 0) < 0) {
        return fail("FBIOSURRENDER", errno);
    }

    int gfx_fd = open("/dev/gfx0", O_RDWR);
    if (gfx_fd < 0) return fail("open /dev/gfx0", errno);

    gfx_view_desc_t vd;
    memset(&vd, 0, sizeof(vd));
    vd.x = 0;
    vd.y = 0;
    vd.w = fbinfo.width;
    vd.h = fbinfo.height;
    if (ioctl(gfx_fd, GFX_CREATE_VIEW, &vd) < 0) {
        return fail("GFX_CREATE_VIEW", errno);
    }

    /* Become the display owner so no other writer may present. */
    if (ioctl(gfx_fd, GFX_ACQUIRE, (uint64_t)0) < 0) {
        return fail("GFX_ACQUIRE", errno);
    }

    gfx_info_t info;
    memset(&info, 0, sizeof(info));
    if (ioctl(gfx_fd, GFX_GET_INFO, &info) < 0) {
        return fail("GFX_GET_INFO", errno);
    }
    if (info.width == 0 || info.height == 0 || info.stride == 0) {
        printf("PIXEL_TEST FAIL: bad gfx info %ux%u stride=%u\n",
               info.width, info.height, info.stride);
        fflush(stdout);
        return 1;
    }

    size_t buf_bytes = (size_t)info.stride * info.height;
    uint32_t *buf = (uint32_t *)calloc(1, buf_bytes);
    if (!buf) return fail("calloc pixel buffer", errno);

    gfx_present_req_t preq;
    memset(&preq, 0, sizeof(preq));
    preq.pixels  = (uint64_t)(uintptr_t)buf;
    preq.stride  = info.stride;
    preq.reserved = 0;

    /* 1. The OLD frame. */
    fill_full(buf, info.stride, info.width, info.height, FRAME_OLD);
    if (ioctl(gfx_fd, GFX_PRESENT, &preq) < 0) {
        return fail("GFX_PRESENT (old frame)", errno);
    }

    /* 2. The NEW frame: every pixel differs from the old one. */
    fill_full(buf, info.stride, info.width, info.height, FRAME_FULL);
    if (ioctl(gfx_fd, GFX_PRESENT, &preq) < 0) {
        return fail("GFX_PRESENT (new frame)", errno);
    }
    printf("FULL_DONE frame=%u width=%u height=%u stride=%u format=%u\n",
           FRAME_FULL, info.width, info.height, info.stride, info.format);
    fflush(stdout);

    /* Stop writing; the host grabs the settled frame now. */
    sleep(HOLD_SECONDS);

    /* 3. The RECTANGLE over the full-frame background: only the
     *    rectangle gets the new seed C, the rest of the buffer stays
     *    the full-frame background B. */
    fill_rect(buf, info.stride, info.width, info.height,
              RECT_X, RECT_Y, RECT_W, RECT_H, FRAME_RECT);

    gfx_present_rect_req_t rreq;
    memset(&rreq, 0, sizeof(rreq));
    rreq.pixels   = (uint64_t)(uintptr_t)buf;
    rreq.stride   = info.stride;
    rreq.reserved = 0;
    rreq.x = RECT_X;
    rreq.y = RECT_Y;
    rreq.w = RECT_W;
    rreq.h = RECT_H;
    if (ioctl(gfx_fd, GFX_PRESENT_RECT, &rreq) < 0) {
        return fail("GFX_PRESENT_RECT", errno);
    }
    printf("RECT_DONE frame=%u x=%u y=%u w=%u h=%u width=%u height=%u stride=%u\n",
           FRAME_RECT, RECT_X, RECT_Y, RECT_W, RECT_H,
           info.width, info.height, info.stride);
    fflush(stdout);

    /* Stop writing again. */
    sleep(HOLD_SECONDS);

    printf("PIXEL_TEST DONE\n");
    fflush(stdout);
    return 0;
}
