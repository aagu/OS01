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

/*
 * 诊断(raw)对象的清单：诊断登记没有公共身份，宿主测试靠下标访问它们。
 */
#define GUI_NATIVE_RAW_MAX 8
static lv_obj_t *g_raw_objects[GUI_NATIVE_RAW_MAX];
static unsigned g_raw_count;

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

    /* 诊断(raw)对象的清单也回到基线。 */
    memset(g_raw_objects, 0, sizeof(g_raw_objects));
    g_raw_count = 0;

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

int gui_native_fixture_query_focus_receiver(gui_native_fixture_t *fx, gui_widget_t *out)
{
    if (!fx || !out) {
        errno = EINVAL;
        return -1;
    }
    out->id = 0;

    if (!fx->input || !fx->platform || !fx->platform->registry) {
        return 0;
    }

    lv_obj_t *focused = lv_group_get_focused(fx->input->lvgl.group);
    if (!focused) {
        return 0;
    }

    /* 真实接收者背后是哪一个身份：遍历本平台的注册表。 */
    gui_widget_registry_t *reg = fx->platform->registry;
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *slot = &reg->slots[i];
        if (slot->state == GUI_WIDGET_SLOT_LIVE && slot->obj == focused) {
            out->id = slot->id;
            break;
        }
    }
    return 0;
}

int gui_native_fixture_query_has_focus_receiver(gui_native_fixture_t *fx)
{
    if (!fx || !fx->input) {
        errno = EINVAL;
        return -1;
    }
    return lv_group_get_focused(fx->input->lvgl.group) != NULL ? 1 : 0;
}

int gui_native_fixture_create_foreign_widget(gui_platform_t *platform, bool own_registry,
                                            gui_widget_t *out)
{
    return gui_widget_test_create_foreign(platform, own_registry ? 1 : 0, out);
}

void gui_native_fixture_free_foreign_widget(gui_platform_t *platform)
{
    gui_widget_test_free_foreign(platform);
}

int gui_native_fixture_query_click_focusable(gui_platform_t *platform, gui_widget_t widget,
                                             bool *out_click_focusable)
{
    if (!platform || !out_click_focusable) {
        errno = EINVAL;
        return -1;
    }
    *out_click_focusable = false;

    lv_obj_t *obj = gui_widget_test_get_obj(platform, widget);
    if (!obj) {
        errno = EINVAL;
        return -1;
    }
    *out_click_focusable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return 0;
}

/* ── 焦点登记表的宿主私有观察 ─────────────────────────────────────── */

int gui_native_fixture_query_focus_order(gui_native_fixture_t *fx, gui_widget_t *out,
                                         int out_capacity)
{
    unsigned i;
    int count = 0;

    if (!fx || !fx->input || !out || out_capacity < 0) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < fx->input->focus.count; i++) {
        const gui_input_focus_entry_t *entry = &fx->input->focus.entries[i];

        if (count >= out_capacity) {
            break;
        }
        out[count].id = entry->native_id;
        count++;
    }
    return count;
}

int gui_native_fixture_query_focus_history(gui_native_fixture_t *fx, bool *out_ever_selected,
                                           bool *out_explicit_clear)
{
    if (!fx || !fx->input) {
        errno = EINVAL;
        return -1;
    }
    if (out_ever_selected) {
        *out_ever_selected = fx->input->focus.ever_selected;
    }
    if (out_explicit_clear) {
        *out_explicit_clear = fx->input->focus.explicit_clear;
    }
    return 0;
}

/* ── 诊断(raw)登记的宿主私有入口 ─────────────────────────────────── */

/*
 * 诊断(raw)登记走的是**内部** hook（gui_input_focus_add）：它没有公共 API，
 * 也不得变成公共 adopt 入口。fixture 在这里替宿主测试触达它。
 */
static lv_obj_t *raw_object(unsigned index)
{
    if (index >= g_raw_count) {
        return NULL;
    }
    return g_raw_objects[index];
}

int gui_native_fixture_raw_is_focused(gui_native_fixture_t *fx, unsigned index)
{
    if (!fx || !fx->input) {
        errno = EINVAL;
        return -1;
    }
    if (raw_object(index) == NULL) {
        return 0;
    }
    return lv_group_get_focused(fx->input->lvgl.group) == raw_object(index) ? 1 : 0;
}

int gui_native_fixture_raw_focus_add_button(gui_native_fixture_t *fx)
{
    lv_obj_t *btn;

    if (!fx || !fx->input || !fx->platform) {
        errno = EINVAL;
        return -1;
    }
    if (g_raw_count >= GUI_NATIVE_RAW_MAX) {
        errno = ENOSPC;
        return -1;
    }
    btn = lv_button_create(lv_display_get_screen_active(fx->platform->disp));
    if (!btn) {
        errno = ENOMEM;
        return -1;
    }
    lv_obj_set_pos(btn, 20 + (int32_t)g_raw_count * 80, 20);
    lv_obj_set_size(btn, 60, 30);

    if (gui_input_focus_add(fx->input, btn) != 0) {
        int saved = errno;

        lv_obj_delete(btn);
        errno = saved;
        return -1;
    }
    g_raw_objects[g_raw_count++] = btn;
    return 0;
}

int gui_native_fixture_raw_focus_add_textarea(gui_native_fixture_t *fx)
{
    lv_obj_t *ta;

    if (!fx || !fx->input || !fx->platform) {
        errno = EINVAL;
        return -1;
    }
    if (g_raw_count >= GUI_NATIVE_RAW_MAX) {
        errno = ENOSPC;
        return -1;
    }
    ta = lv_textarea_create(lv_display_get_screen_active(fx->platform->disp));
    if (!ta) {
        errno = ENOMEM;
        return -1;
    }
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_pos(ta, 20 + (int32_t)g_raw_count * 80, 80);
    lv_obj_set_size(ta, 120, 30);

    if (gui_input_focus_add(fx->input, ta) != 0) {
        int saved = errno;

        lv_obj_delete(ta);
        errno = saved;
        return -1;
    }
    g_raw_objects[g_raw_count++] = ta;
    return 0;
}

const char *gui_native_fixture_raw_textarea_text(gui_native_fixture_t *fx, unsigned index)
{
    lv_obj_t *obj = raw_object(index);

    (void)fx;
    if (!obj || !lv_obj_check_type(obj, &lv_textarea_class)) {
        return NULL;
    }
    return lv_textarea_get_text(obj);
}

int gui_native_fixture_raw_click(gui_native_fixture_t *fx, unsigned index)
{
    lv_obj_t *obj = raw_object(index);
    lv_area_t area;
    mouse_event_t ev;

    if (!fx || !fx->input || !obj) {
        errno = EINVAL;
        return -1;
    }
    lv_obj_get_coords(obj, &area);

    /* 指针的位移是相对的：先归到左上角，再移到目标中心。 */
    memset(&ev, 0, sizeof(ev));
    ev.dx = INT16_MIN;
    ev.dy = INT16_MIN;
    if (gui_native_fixture_mouse(fx, &ev, 1) != 0) {
        return -1;
    }
    if (gui_native_fixture_step(fx, 50) != 0) {
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.dx = (int16_t)(area.x1 + (area.x2 - area.x1) / 2);
    ev.dy = (int16_t)(area.y1 + (area.y2 - area.y1) / 2);
    if (gui_native_fixture_mouse(fx, &ev, 1) != 0) {
        return -1;
    }
    if (gui_native_fixture_step(fx, 50) != 0) {
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.buttons = 0x01u;
    if (gui_native_fixture_mouse(fx, &ev, 1) != 0) {
        return -1;
    }
    if (gui_native_fixture_step(fx, 50) != 0) {
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    if (gui_native_fixture_mouse(fx, &ev, 1) != 0) {
        return -1;
    }
    if (gui_native_fixture_step(fx, 50) != 0) {
        return -1;
    }
    return 0;
}

int gui_native_fixture_raw_delete_focused(gui_native_fixture_t *fx)
{
    lv_obj_t *focused;
    unsigned i;

    if (!fx || !fx->input) {
        errno = EINVAL;
        return -1;
    }
    focused = lv_group_get_focused(fx->input->lvgl.group);
    if (!focused) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < g_raw_count; i++) {
        if (g_raw_objects[i] == focused) {
            lv_obj_delete(focused);
            g_raw_objects[i] = NULL;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

int gui_native_fixture_pool_bytes(gui_native_fixture_t *fx, uint64_t *out_total,                                  uint64_t *out_free, uint64_t *out_max_used)
{
    if (!fx) {
        errno = EINVAL;
        return -1;
    }

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if (out_total) {
        *out_total = (uint64_t)mon.total_size;
    }
    if (out_free) {
        *out_free = (uint64_t)mon.free_size;
    }
    if (out_max_used) {
        *out_max_used = (uint64_t)mon.max_used;
    }
    return 0;
}

/* ── 删除期间的创建探针 ──────────────────────────────────────────────
 *
 * 探针挂在 victim **自己**身上：原生删除事件在删除事务内部送达它，
 * 那正是"删除回调期间创建控件"的真实场景。
 */
static gui_platform_t *g_delete_probe_platform;
static gui_widget_t g_delete_probe_parent;
static lv_obj_t *g_delete_probe_victim_obj;
static gui_widget_t *g_delete_probe_out;
static bool g_delete_probe_armed;

static void gui_native_delete_probe_cb(lv_event_t *event)
{
    if (!g_delete_probe_armed) {
        return;
    }
    /* 只对 victim 自己的删除事件反应。 */
    if (lv_event_get_target(event) != g_delete_probe_victim_obj) {
        return;
    }
    g_delete_probe_armed = false;

    gui_widget_t created;
    created.id = 0;
    if (gui_widget_create(g_delete_probe_platform, g_delete_probe_parent, GUI_WIDGET_CONTAINER,
                          &created) == 0 && g_delete_probe_out) {
        *g_delete_probe_out = created;
    }
}

int gui_native_fixture_create_during_delete(gui_platform_t *platform, gui_widget_t victim,
                                            gui_widget_t parent, gui_widget_t *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    out->id = 0;

    lv_obj_t *victim_obj = gui_widget_test_get_obj(platform, victim);
    if (!victim_obj) {
        errno = EINVAL;
        return -1;
    }

    g_delete_probe_platform = platform;
    g_delete_probe_parent = parent;
    g_delete_probe_victim_obj = victim_obj;
    g_delete_probe_out = out;
    g_delete_probe_armed = true;

    if (!lv_obj_add_event_cb(victim_obj, gui_native_delete_probe_cb, LV_EVENT_DELETE, NULL)) {
        g_delete_probe_armed = false;
        errno = ENOMEM;
        return -1;
    }
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
