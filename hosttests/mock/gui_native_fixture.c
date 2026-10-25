/*
 * gui_native_fixture.c — 宿主私有的原生控件 fixture（GUI-2b）
 *
 * fixture 只做三件事：建平台、（可选）建合成输入、把宿主注入桥接起来。
 * 它**不**模拟任何被测行为：平台、输入、解码器与后端对象都是真的。
 */

#include "gui_native_fixture.h"

#include "gui_test_runtime.h"
#include <gui_input_private.h>

#include <errno.h>
#include <string.h>

int gui_native_fixture_create(gui_native_fixture_t *fx, bool with_input)
{
    if (!fx) {
        errno = EINVAL;
        return -1;
    }
    memset(fx, 0, sizeof(*fx));
    fx->root = GUI_WIDGET_NONE;

    /* 先复位宿主 mock：假时钟、计数与故障注入都回到基线。 */
    gui_test_reset();

    gui_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = GUI_MODE_OFFSCREEN;
    cfg.width = 640;
    cfg.height = 480;
    cfg.buffer_lines = 40;

    gui_platform_t *platform = gui_platform_create(&cfg);
    if (!platform) {
        return -1; /* errno 已由平台设置 */
    }
    fx->platform = platform;

    if (with_input) {
        gui_input_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        icfg.mode = GUI_INPUT_SYNTHETIC;
        icfg.require_mouse = false;

        gui_input_t *input = gui_input_create(platform, &icfg);
        if (!input) {
            int saved = errno;
            gui_platform_destroy(platform);
            fx->platform = NULL;
            errno = saved;
            return -1;
        }
        fx->input = input;
    }

    return 0;
}

void gui_native_fixture_destroy(gui_native_fixture_t *fx)
{
    if (!fx) {
        return;
    }
    if (fx->input) {
        gui_input_destroy(fx->input);
        fx->input = NULL;
    }
    if (fx->platform) {
        gui_platform_destroy(fx->platform);
        fx->platform = NULL;
    }
    fx->root = GUI_WIDGET_NONE;
    fx->delay = 0;
}

int gui_native_fixture_step(gui_native_fixture_t *fx, uint64_t advance_ms)
{
    if (!fx || !fx->platform) {
        errno = EINVAL;
        return -1;
    }

    gui_test_advance_now(advance_ms);

    uint32_t delay = 0;
    gui_status_t status = gui_platform_step(fx->platform, &delay);
    if (status != GUI_STATUS_OK) {
        errno = EIO;
        return -1;
    }
    fx->delay = delay;
    return 0;
}

int gui_native_fixture_keys(gui_native_fixture_t *fx, const uint8_t *bytes, size_t len)
{
    if (!fx || !fx->input) {
        errno = EINVAL;
        return -1;
    }
    return gui_input_test_feed_keybytes(fx->input, bytes, len);
}

int gui_native_fixture_mouse(gui_native_fixture_t *fx, const mouse_event_t *events, size_t count)
{
    if (!fx || !fx->input) {
        errno = EINVAL;
        return -1;
    }
    return gui_input_test_feed_mouse(fx->input, events, count);
}
