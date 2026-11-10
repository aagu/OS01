#ifndef _DOOM_VIDEO_H
#define _DOOM_VIDEO_H

#include <stdint.h>
#include <gfx.h>

typedef struct doom_video {
    gfx_handle_t *handle;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t scale;
    int32_t offset_x;
    int32_t offset_y;
    uint32_t scaled_w;
    uint32_t scaled_h;
    uint32_t *scale_buf;
    int initial_present_pending;
    int acquired;
} doom_video_t;

int doom_video_open(doom_video_t *v, unsigned requested_scale);
int doom_video_draw(doom_video_t *v, const uint32_t *pixels);
void doom_video_close(doom_video_t *v);
gfx_handle_t *doom_video_handle(doom_video_t *v);

#endif /* _DOOM_VIDEO_H */
