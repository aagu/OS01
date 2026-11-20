#include "fb_mode_fixture.h"
#include <driver/bga.h>
#include <errno.h>
#include <stddef.h>

static const struct fb_set_mode_req *fixture_modes;
static uint32_t fixture_mode_count;

int fb_mode_fixture_install(uint32_t *addr, uint64_t mapped_size,
                            const struct fb_set_mode_req *modes, uint32_t count)
{
    if (!modes || count == 0 || count > FB_MAX_MODES)
        return -EINVAL;

    const bga_caps_t caps = { .vram_bytes = mapped_size };
    fixture_modes = modes;
    fixture_mode_count = count;
    int result = fb_install_backend(&caps, addr, mapped_size);
    fixture_modes = NULL;
    fixture_mode_count = 0;
    return result;
}

uint32_t bga_filter_modes(const bga_caps_t *caps, uint64_t mapped_size,
                          struct fb_info out[FB_MAX_MODES])
{
    (void)caps;
    (void)mapped_size;
    for (uint32_t index = 0; index < fixture_mode_count; index++) {
        const struct fb_set_mode_req *mode = &fixture_modes[index];
        out[index] = (struct fb_info) {
            .width = mode->width,
            .height = mode->height,
            .stride = mode->width * 4,
            .bpp = 32,
            .format = FB_FORMAT_RGB32,
        };
    }
    return fixture_mode_count;
}

enum bga_result bga_apply_mode(const struct fb_info *target)
{
    (void)target;
    return BGA_APPLIED;
}
