/*
 * gui_native_fixture.c — 宿主私有的原生控件 fixture（GUI-2b）
 *
 * fixture 只做三件事：建平台、（可选）建合成输入、把宿主注入桥接起来。
 * 它**不**模拟任何被测行为：平台、输入、解码器与后端对象都是真的。
 */

#include "gui_native_fixture.h"

#include "gui_test_runtime.h"
#include <gui_input_private.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>
#include <gui_widget_private.h>
#include <lvgl/lvgl.h>

#include <errno.h>
#include <string.h>

/* 宿主视图与 libgui 内部常量必须一一对应：漂了就说明 fixture 说谎了。 */
_Static_assert(GUI_NATIVE_FLAG_VISIBLE == GUI_WIDGET_FLAG_VISIBLE, "visible flag drift");
_Static_assert(GUI_NATIVE_FLAG_ENABLED == GUI_WIDGET_FLAG_ENABLED, "enabled flag drift");
_Static_assert(GUI_NATIVE_FLAG_CLICKABLE == GUI_WIDGET_FLAG_CLICKABLE, "clickable flag drift");
_Static_assert(GUI_NATIVE_SLOT_FREE == GUI_WIDGET_SLOT_FREE, "slot state drift");
_Static_assert(GUI_NATIVE_SLOT_RESERVED == GUI_WIDGET_SLOT_RESERVED, "slot state drift");
_Static_assert(GUI_NATIVE_SLOT_LIVE == GUI_WIDGET_SLOT_LIVE, "slot state drift");
_Static_assert(GUI_NATIVE_SLOT_DESTROYING == GUI_WIDGET_SLOT_DESTROYING, "slot state drift");
_Static_assert(GUI_NATIVE_CAPACITY == GUI_WIDGET_REGISTRY_CAPACITY, "capacity drift");

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

/* ── 宿主私有表查询 ───────────────────────────────────────────────── */

int gui_native_fixture_query_slot(gui_platform_t *platform, gui_widget_t widget,
                                  gui_native_slot_info_t *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    gui_widget_slot_t slot;
    if (gui_widget_test_query_slot(platform, widget, &slot) != 0) {
        return -1;
    }
    if (slot.id == 0 && slot.state == GUI_WIDGET_SLOT_FREE) {
        return 0; /* 身份不存在 */
    }

    out->state = (int)slot.state;
    out->index = slot.index;
    out->id = slot.id;
    out->parent_id = slot.parent_id;
    out->kind = (int)slot.kind;
    out->has_handler = slot.handler != NULL;
    out->has_userdata = slot.userdata != NULL;
    out->has_text = slot.text != NULL;
    out->flags = slot.flags;
    out->geom_x = slot.geom_x;
    out->geom_y = slot.geom_y;
    out->geom_width = slot.geom_width;
    out->geom_height = slot.geom_height;
    return 0;
}

int gui_native_fixture_query_registry(gui_platform_t *platform, gui_native_registry_info_t *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    uint32_t count = 0;
    uint32_t peak = 0;
    if (gui_widget_test_get_counters(platform, &count, &peak) != 0) {
        return -1;
    }
    out->count = count;
    out->peak = peak;
    out->capacity = GUI_WIDGET_REGISTRY_CAPACITY;
    return 0;
}

int gui_native_fixture_plant_slot(gui_platform_t *platform, gui_widget_t widget,
                                  uint32_t stale_flags, const char *stale_text)
{
    return gui_widget_test_plant_slot(platform, widget, stale_flags, stale_text);
}

void gui_native_fixture_set_identity_last_id(uint64_t last_id)
{
    gui_widget_test_set_last_id(last_id);
}

uint64_t gui_native_fixture_get_identity_last_id(void)
{
    return gui_widget_test_get_last_id();
}

int gui_native_fixture_pool_free(gui_platform_t *platform, size_t *out_free_bytes)
{
    (void)platform;
    if (!out_free_bytes) {
        errno = EINVAL;
        return -1;
    }
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    *out_free_bytes = monitor.free_size;
    return 0;
}

int gui_native_fixture_backend_delete(gui_platform_t *platform, gui_widget_t widget)
{
    return gui_widget_test_backend_delete(platform, widget);
}

int gui_native_fixture_set_focus_visual(gui_native_fixture_t *fx, gui_widget_t widget, bool focused)
{
    if (!fx || !fx->platform) {
        errno = EINVAL;
        return -1;
    }

    lv_obj_t *obj = gui_widget_test_get_obj(fx->platform, widget);
    if (!obj) {
        errno = EINVAL;
        return -1;
    }

    /* 只改后端的焦点视觉状态位：输入登记与公共焦点 API 都不在这里。 */
    if (focused) {
        lv_obj_add_state(obj, LV_STATE_FOCUSED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_FOCUSED);
    }
    lv_obj_invalidate(obj);
    return 0;
}

int gui_native_fixture_query_backend(gui_platform_t *platform, gui_widget_t widget,
                                     gui_native_backend_info_t *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    lv_obj_t *obj = gui_widget_test_get_obj(platform, widget);
    if (!obj) {
        errno = EINVAL;
        return -1;
    }

    out->clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_display_t *disp = lv_obj_get_display(obj);
    lv_display_t *platform_disp = gui_platform_get_lv_display(platform);
    out->platform_display = (disp != NULL && disp == platform_disp);
    out->default_display = (disp != NULL && disp == lv_display_get_default());

    out->in_group = (lv_obj_get_group(obj) != NULL);
    return 0;
}

int gui_native_fixture_set_default_display_none(void)
{
    lv_display_set_default(NULL);
    return 0;
}

int gui_native_fixture_restore_default_display(gui_platform_t *platform)
{
    lv_display_t *platform_disp = gui_platform_get_lv_display(platform);
    if (!platform_disp) {
        errno = EINVAL;
        return -1;
    }
    lv_display_set_default(platform_disp);
    return 0;
}
