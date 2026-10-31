/*
 * hosttests/cases/test_fb_uapi.c — Framebuffer mode control ABI layout tests.
 *
 * Pins the byte layout, alignment, and ioctl numbers defined in
 * kernel/include/uapi/fb.h (Spec §5).
 */
#include <stddef.h>
#include <stdint.h>
#include <test_framework.h>
#include <uapi/fb.h>

TEST_FUNC(test_fb_uapi_layout) {
    /* 1. struct sizes & alignments */
    assert_eq(20, sizeof(struct fb_info));
    assert_eq(332, sizeof(struct fb_modes_req));
    assert_eq(4, _Alignof(struct fb_modes_req));
    assert_eq(12, sizeof(struct fb_set_mode_req));
    assert_eq(32, sizeof(struct fb_state));

    /* 2. struct fb_state member offsets */
    assert_eq(0, offsetof(struct fb_state, info));
    assert_eq(20, offsetof(struct fb_state, reserved));
    assert_eq(24, offsetof(struct fb_state, generation));

    /* 3. Original fb_info field order and offsets */
    assert_eq(0, offsetof(struct fb_info, width));
    assert_eq(4, offsetof(struct fb_info, height));
    assert_eq(8, offsetof(struct fb_info, stride));
    assert_eq(12, offsetof(struct fb_info, bpp));
    assert_eq(16, offsetof(struct fb_info, format));

    /* 4. struct fb_modes_req member offsets */
    assert_eq(0, offsetof(struct fb_modes_req, capacity));
    assert_eq(4, offsetof(struct fb_modes_req, count));
    assert_eq(8, offsetof(struct fb_modes_req, total));
    assert_eq(12, offsetof(struct fb_modes_req, modes));

    /* 5. struct fb_set_mode_req member offsets */
    assert_eq(0, offsetof(struct fb_set_mode_req, width));
    assert_eq(4, offsetof(struct fb_set_mode_req, height));
    assert_eq(8, offsetof(struct fb_set_mode_req, bpp));

    /* 6. Five ioctl command numbers */
    assert_eq(0x00004601, FBIOSURRENDER);
    assert_eq(0x00004602, FBIOGET_MODES);
    assert_eq(0x00004603, FBIOSET_MODE);
    assert_eq(0x00004604, FBIOGET_CURR_MODE);
    assert_eq(0x00004605, FBIOGET_STATE);

    /* 7. Constants */
    assert_eq(16, FB_MAX_MODES);
    assert_eq(0, FB_FORMAT_RGB32);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fb_uapi_layout),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
