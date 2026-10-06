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

static int g_failed = 0;

#define CHECK_TRUE(cond) do { \
    if (!(cond)) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_EQ(exp, act) do { \
    long _e = (long)(exp); \
    long _a = (long)(act); \
    if (_e != _a) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected %ld, got %ld)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

TEST_FUNC(test_fb_uapi_layout) {
    /* 1. struct sizes & alignments */
    CHECK_EQ(20, sizeof(struct fb_info));
    CHECK_EQ(332, sizeof(struct fb_modes_req));
    CHECK_EQ(4, _Alignof(struct fb_modes_req));
    CHECK_EQ(12, sizeof(struct fb_set_mode_req));
    CHECK_EQ(32, sizeof(struct fb_state));

    /* 2. struct fb_state member offsets */
    CHECK_EQ(0, offsetof(struct fb_state, info));
    CHECK_EQ(20, offsetof(struct fb_state, reserved));
    CHECK_EQ(24, offsetof(struct fb_state, generation));

    /* 3. Original fb_info field order and offsets */
    CHECK_EQ(0, offsetof(struct fb_info, width));
    CHECK_EQ(4, offsetof(struct fb_info, height));
    CHECK_EQ(8, offsetof(struct fb_info, stride));
    CHECK_EQ(12, offsetof(struct fb_info, bpp));
    CHECK_EQ(16, offsetof(struct fb_info, format));

    /* 4. struct fb_modes_req member offsets */
    CHECK_EQ(0, offsetof(struct fb_modes_req, capacity));
    CHECK_EQ(4, offsetof(struct fb_modes_req, count));
    CHECK_EQ(8, offsetof(struct fb_modes_req, total));
    CHECK_EQ(12, offsetof(struct fb_modes_req, modes));

    /* 5. struct fb_set_mode_req member offsets */
    CHECK_EQ(0, offsetof(struct fb_set_mode_req, width));
    CHECK_EQ(4, offsetof(struct fb_set_mode_req, height));
    CHECK_EQ(8, offsetof(struct fb_set_mode_req, bpp));

    /* 6. Five ioctl command numbers */
    CHECK_EQ(0x00004601, FBIOSURRENDER);
    CHECK_EQ(0x00004602, FBIOGET_MODES);
    CHECK_EQ(0x00004603, FBIOSET_MODE);
    CHECK_EQ(0x00004604, FBIOGET_CURR_MODE);
    CHECK_EQ(0x00004605, FBIOGET_STATE);

    /* 7. Constants */
    CHECK_EQ(16, FB_MAX_MODES);
    CHECK_EQ(0, FB_FORMAT_RGB32);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fb_uapi_layout),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    if (g_failed > 0) {
        printf("test_fb_uapi: %d failures\n", g_failed);
        return 1;
    }
    return 0;
}
