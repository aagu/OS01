#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/fb.h>
#include <gfx.h>
#include "gfx_client_policy.h"
#include "doom_video.h"

#ifdef OS01_DOOM_DIAG
#include "diag.h"
#endif

int doom_video_open(doom_video_t *v, unsigned requested_scale)
{
    if (!v) {
        errno = EINVAL;
        return -1;
    }
    memset(v, 0, sizeof(*v));

    int fb_fd = open("/dev/fb", O_RDONLY);
    if (fb_fd < 0) {
        return -1;
    }

    struct fb_info info;
    ssize_t n = read(fb_fd, &info, sizeof(info));
    int read_err = errno;
    close(fb_fd);

    if (n != (ssize_t)sizeof(info)) {
        errno = read_err ? read_err : EIO;
        return -1;
    }

    if (info.bpp != 32 || info.format != FB_FORMAT_RGB32) {
        errno = ENODEV;
        return -1;
    }

    if (info.width < 320 || info.height < 200 || info.width > 16384 || info.height > 16384) {
        errno = EINVAL;
        return -1;
    }

    unsigned s;
    if (requested_scale == 0) {
        unsigned s_w = info.width / 320;
        unsigned s_h = info.height / 200;
        s = s_w < s_h ? s_w : s_h;
        if (s > 3) s = 3;
        if (s < 1) {
            errno = EINVAL;
            return -1;
        }
    } else {
        if (requested_scale < 1 || requested_scale > 3) {
            errno = EINVAL;
            return -1;
        }
        if (320 * requested_scale > info.width || 200 * requested_scale > info.height) {
            errno = EINVAL;
            return -1;
        }
        s = requested_scale;
    }

    v->screen_w = info.width;
    v->screen_h = info.height;
    v->scale = s;
    v->scaled_w = 320 * s;
    v->scaled_h = 200 * s;
    v->offset_x = (int32_t)(info.width - v->scaled_w) / 2;
    v->offset_y = (int32_t)(info.height - v->scaled_h) / 2;

    v->scale_buf = malloc((size_t)v->scaled_w * v->scaled_h * sizeof(uint32_t));
    if (!v->scale_buf) {
        errno = ENOMEM;
        return -1;
    }

    v->handle = gfx_open(0, 0, info.width, info.height);
    if (!v->handle) {
        int err = errno;
        free(v->scale_buf);
        v->scale_buf = NULL;
        errno = err;
        return -1;
    }

    if (gfx_acquire(v->handle) < 0) {
        int err = errno;
        gfx_close(v->handle);
        v->handle = NULL;
        free(v->scale_buf);
        v->scale_buf = NULL;
        errno = err;
        return -1;
    }
    v->acquired = 1;

    gfx_fill_rect(v->handle, 0, 0, info.width, info.height, 0x00000000);

    v->initial_present_pending = 1;
    if (gfx_present(v->handle) == 0) {
        v->initial_present_pending = 0;
    } else if (errno != EAGAIN) {
        int err = errno;
        gfx_release(v->handle);
        gfx_close(v->handle);
        v->handle = NULL;
        free(v->scale_buf);
        v->scale_buf = NULL;
        v->acquired = 0;
        errno = err;
        return -1;
    }

    return 0;
}

int doom_video_draw(doom_video_t *v, const uint32_t *pixels)
{
    if (!v || !v->handle || !v->scale_buf || !pixels) {
        errno = EINVAL;
        return -1;
    }

    uint32_t s = v->scale;
    for (uint32_t y = 0; y < 200; y++) {
        const uint32_t *src_row = &pixels[y * 320];
        uint32_t *dst_first_row = &v->scale_buf[(y * s) * (320 * s)];
        if (s == 1) {
            memcpy(dst_first_row, src_row, 320 * sizeof(uint32_t));
        } else if (s == 2) {
            for (uint32_t x = 0; x < 320; x++) {
                uint32_t c = src_row[x];
                dst_first_row[2 * x] = c;
                dst_first_row[2 * x + 1] = c;
            }
            memcpy(&v->scale_buf[(y * 2 + 1) * 640], dst_first_row, 640 * sizeof(uint32_t));
        } else if (s == 3) {
            for (uint32_t x = 0; x < 320; x++) {
                uint32_t c = src_row[x];
                dst_first_row[3 * x] = c;
                dst_first_row[3 * x + 1] = c;
                dst_first_row[3 * x + 2] = c;
            }
            memcpy(&v->scale_buf[(y * 3 + 1) * 960], dst_first_row, 960 * sizeof(uint32_t));
            memcpy(&v->scale_buf[(y * 3 + 2) * 960], dst_first_row, 960 * sizeof(uint32_t));
        }
    }

    gfx_sprite_blit(v->handle, v->offset_x, v->offset_y,
                    v->scale_buf, v->scaled_w * sizeof(uint32_t),
                    v->scaled_w, v->scaled_h, false, 0);

    int prc;
    if (v->initial_present_pending) {
        prc = gfx_present(v->handle);
        if (prc == 0) {
            v->initial_present_pending = 0;
#ifdef OS01_DOOM_DIAG
            doom_diag_first_frame();
#endif
            return 0;
        }
    } else {
        prc = gfx_present_rect(v->handle, (uint32_t)v->offset_x, (uint32_t)v->offset_y,
                               v->scaled_w, v->scaled_h);
        if (prc == 0) {
#ifdef OS01_DOOM_DIAG
            doom_diag_present_stat(0, 0, 0);
#endif
            return 0;
        }
    }

    int err = errno;
#ifdef OS01_DOOM_DIAG
    doom_diag_present_stat(prc, err, 0);
#endif
    gfx_client_verdict_t verdict = gfx_client_classify(err);
    if (verdict == GFX_CLIENT_RETRY) {
        return 1;
    }
    errno = err;
    return -1;
}

void doom_video_close(doom_video_t *v)
{
    if (!v) return;

    if (v->handle) {
        if (v->acquired) {
            gfx_release(v->handle);
            v->acquired = 0;
        }
        gfx_close(v->handle);
        v->handle = NULL;
    }
    if (v->scale_buf) {
        free(v->scale_buf);
        v->scale_buf = NULL;
    }
}

gfx_handle_t *doom_video_handle(doom_video_t *v)
{
    return v ? v->handle : NULL;
}
