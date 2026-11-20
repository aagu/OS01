#ifndef OS01_FB_MODE_FIXTURE_H
#define OS01_FB_MODE_FIXTURE_H

#include <uapi/fb.h>

int fb_mode_fixture_install(uint32_t *addr, uint64_t mapped_size,
                            const struct fb_set_mode_req *modes, uint32_t count);

#endif
