/*
 * theme_apply.c — 把 Material 令牌表落地为 LVGL 主题（GUI-3）
 *
 * 本文件是令牌表与 LVGL 之间的**唯一**接缝：所有颜色、字号、间距、圆角
 * 与 elevation 都来自 gui_theme.h，不在这里出现任何字面量。
 *
 * 主题自建（不继承 LVGL 默认主题）：默认主题的圆角 / 间距 / 状态色与
 * Material 令牌不一致，继承会让「统一」落空。
 *
 * 覆盖的控件类：OBJ / LABEL / BUTTON / SWITCH / CHECKBOX / SLIDER / BAR /
 * LIST / SCROLLBAR / TEXTAREA。
 */

#include <gui/gui_theme.h>
#include <gui_theme_private.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/themes/lv_theme_private.h>

#include <stdbool.h>
#include <stdint.h>

/* ── 令牌 → LVGL 的转换小工具 ───────────────────────────────────── */

static lv_color_t tok_color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static const lv_font_t *tok_font(gui_theme_font_t role)
{
    switch (role) {
    case GUI_THEME_FONT_DISPLAY:  return &lv_font_montserrat_24;
    case GUI_THEME_FONT_HEADLINE: return &lv_font_montserrat_20;
    case GUI_THEME_FONT_TITLE:    return &lv_font_montserrat_16;
    case GUI_THEME_FONT_BODY:     return &lv_font_montserrat_14;
    case GUI_THEME_FONT_LABEL:    return &lv_font_montserrat_12;
    default:                      return LV_FONT_DEFAULT;
    }
}

/* ── 样式集合 ───────────────────────────────────────────────────── */

typedef struct {
    lv_style_t scr;          /* 屏幕底：壁纸 */
    lv_style_t card;         /* 卡片：elevation 2 */
    lv_style_t card_hover;   /* 卡片 hover：elevation 3 */
    lv_style_t card_pressed; /* 卡片 pressed：elevation 4 */
    lv_style_t label;
    lv_style_t label_muted;
    lv_style_t btn;
    lv_style_t btn_hover;
    lv_style_t btn_pressed;
    lv_style_t btn_disabled;
    lv_style_t btn_focused;
    lv_style_t focus_ring;
    lv_style_t sw;
    lv_style_t sw_knob;
    lv_style_t sw_checked;
    lv_style_t sw_checked_knob;
    lv_style_t checkbox;
    lv_style_t checkbox_checked;
    lv_style_t slider;
    lv_style_t slider_indicator;
    lv_style_t bar;
    lv_style_t bar_indicator;
    lv_style_t list;
    lv_style_t list_item;
    lv_style_t scrollbar;
    lv_style_t textarea;
} theme_styles_t;

/*
 * 样式块与主题**同生共死**：lv_style_t 内部的值缓冲是 LVGL 内存池里的
 * 堆分配，lv_deinit() 会把整个池释放掉。若把样式放在文件静态区，第二次
 * gui_platform_create() 之后这些指针就是悬垂的，解析出来的颜色会变成
 * 别的数据。因此样式随主题一起分配、随主题一起释放。
 */

/* ── 样式初始化 ─────────────────────────────────────────────────── */

static void style_init(theme_styles_t *s)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    /* 屏幕：壁纸 = surface，无边框无圆角。 */
    lv_style_init(&s->scr);
    lv_style_set_bg_color(&s->scr, tok_color(c->surface));
    lv_style_set_bg_opa(&s->scr, LV_OPA_COVER);
    lv_style_set_border_width(&s->scr, 0);
    lv_style_set_radius(&s->scr, 0);
    lv_style_set_text_color(&s->scr, tok_color(c->on_surface));
    lv_style_set_text_font(&s->scr, tok_font(GUI_THEME_FONT_BODY));

    /* 卡片：elevation 2。 */
    lv_style_init(&s->card);
    lv_style_set_bg_color(&s->card, tok_color(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2)));
    lv_style_set_bg_opa(&s->card, LV_OPA_COVER);
    lv_style_set_radius(&s->card, gui_theme_radius(GUI_THEME_RADIUS_LG));
    lv_style_set_border_color(&s->card, tok_color(c->outline_variant));
    lv_style_set_border_width(&s->card, 1);
    lv_style_set_pad_all(&s->card, gui_theme_space(GUI_THEME_SPACE_MD));
    lv_style_set_pad_row(&s->card, gui_theme_space(GUI_THEME_SPACE_SM));
    lv_style_set_text_color(&s->card, tok_color(c->on_surface));
    lv_style_set_text_font(&s->card, tok_font(GUI_THEME_FONT_BODY));
    lv_style_set_shadow_color(&s->card, tok_color(c->shadow));
    lv_style_set_shadow_width(&s->card,
                              gui_theme_elevation_shadow_width(GUI_THEME_ELEVATION_2));
    lv_style_set_shadow_offset_y(&s->card,
                                 gui_theme_elevation_shadow_offset_y(GUI_THEME_ELEVATION_2));
    lv_style_set_shadow_opa(&s->card,
                            gui_theme_elevation_shadow_opa(GUI_THEME_ELEVATION_2));

    /* 卡片 hover：表面提亮一级（elevation 3）。 */
    lv_style_init(&s->card_hover);
    lv_style_set_bg_color(&s->card_hover,
                          tok_color(gui_theme_elevation_surface(GUI_THEME_ELEVATION_3)));
    lv_style_set_shadow_width(&s->card_hover,
                              gui_theme_elevation_shadow_width(GUI_THEME_ELEVATION_3));
    lv_style_set_shadow_offset_y(&s->card_hover,
                                 gui_theme_elevation_shadow_offset_y(GUI_THEME_ELEVATION_3));
    lv_style_set_shadow_opa(&s->card_hover,
                            gui_theme_elevation_shadow_opa(GUI_THEME_ELEVATION_3));

    /* 卡片 pressed：表面压暗一级（elevation 4）。 */
    lv_style_init(&s->card_pressed);
    lv_style_set_bg_color(&s->card_pressed,
                          tok_color(gui_theme_elevation_surface(GUI_THEME_ELEVATION_4)));
    lv_style_set_shadow_width(&s->card_pressed,
                              gui_theme_elevation_shadow_width(GUI_THEME_ELEVATION_4));
    lv_style_set_shadow_offset_y(&s->card_pressed,
                                 gui_theme_elevation_shadow_offset_y(GUI_THEME_ELEVATION_4));
    lv_style_set_shadow_opa(&s->card_pressed,
                            gui_theme_elevation_shadow_opa(GUI_THEME_ELEVATION_4));

    /* 文本：默认 / 次要。 */
    lv_style_init(&s->label);
    lv_style_set_text_color(&s->label, tok_color(c->on_surface));
    lv_style_set_text_font(&s->label, tok_font(GUI_THEME_FONT_BODY));

    lv_style_init(&s->label_muted);
    lv_style_set_text_color(&s->label_muted, tok_color(c->on_surface_variant));
    lv_style_set_text_font(&s->label_muted, tok_font(GUI_THEME_FONT_LABEL));

    /* 主按钮：默认 / hover / pressed / disabled。 */
    lv_style_init(&s->btn);
    lv_style_set_bg_color(&s->btn, tok_color(c->primary));
    lv_style_set_bg_opa(&s->btn, LV_OPA_COVER);
    lv_style_set_radius(&s->btn, gui_theme_radius(GUI_THEME_RADIUS_MD));
    lv_style_set_border_width(&s->btn, 0);
    lv_style_set_pad_hor(&s->btn, gui_theme_space(GUI_THEME_SPACE_MD));
    lv_style_set_pad_ver(&s->btn, gui_theme_space(GUI_THEME_SPACE_SM));
    lv_style_set_text_color(&s->btn, tok_color(c->on_primary));
    lv_style_set_text_font(&s->btn, tok_font(GUI_THEME_FONT_TITLE));

    lv_style_init(&s->btn_hover);
    lv_style_set_bg_color(&s->btn_hover, tok_color(c->secondary));

    lv_style_init(&s->btn_pressed);
    lv_style_set_bg_color(&s->btn_pressed, tok_color(c->primary_container));
    lv_style_set_text_color(&s->btn_pressed, tok_color(c->on_primary_container));

    lv_style_init(&s->btn_disabled);
    lv_style_set_bg_color(&s->btn_disabled, tok_color(c->surface_variant));
    lv_style_set_text_color(&s->btn_disabled, tok_color(c->on_surface_variant));

    /*
     * 按钮聚焦态：选中容器语义（primary_container 底 + on_primary_container
     * 文字）。
     *
     * 默认 ACTION 按钮的普通态底色**就是** primary：焦点环也是 primary，
     * 同色时环画出来也看不见。聚焦态因此换用同一主题里的
     * primary_container，环（primary 2px）与填充（primary_container）才
     * 互相可区分。普通态不变——像素保真依赖它。
     */
    lv_style_init(&s->btn_focused);
    lv_style_set_bg_color(&s->btn_focused, tok_color(c->primary_container));
    lv_style_set_text_color(&s->btn_focused, tok_color(c->on_primary_container));

    /*
     * 焦点环：2px primary 轮廓。
     *
     * 环**自带**透明度（LV_OPA_COVER）：外观层在 bordered=false 时只把
     * border_width 置 0（零宽边框即"零普通边框"），不会把 border_opa 换成
     * 透明——否则这里的环会被普通态的透明度吃掉，整个环不可见。
     */
    lv_style_init(&s->focus_ring);
    lv_style_set_border_color(&s->focus_ring, tok_color(c->primary));
    lv_style_set_border_width(&s->focus_ring, 2);
    lv_style_set_border_opa(&s->focus_ring, LV_OPA_COVER);
    lv_style_set_radius(&s->focus_ring, gui_theme_radius(GUI_THEME_RADIUS_MD));

    /* 开关。 */
    lv_style_init(&s->sw);
    lv_style_set_bg_color(&s->sw, tok_color(c->surface_variant));
    lv_style_set_bg_opa(&s->sw, LV_OPA_COVER);
    lv_style_set_radius(&s->sw, gui_theme_radius(GUI_THEME_RADIUS_FULL));

    lv_style_init(&s->sw_knob);
    lv_style_set_bg_color(&s->sw_knob, tok_color(c->outline));
    lv_style_set_radius(&s->sw_knob, gui_theme_radius(GUI_THEME_RADIUS_FULL));

    lv_style_init(&s->sw_checked);
    lv_style_set_bg_color(&s->sw_checked, tok_color(c->primary));

    lv_style_init(&s->sw_checked_knob);
    lv_style_set_bg_color(&s->sw_checked_knob, tok_color(c->on_primary));

    /* 复选框。 */
    lv_style_init(&s->checkbox);
    lv_style_set_bg_color(&s->checkbox, tok_color(c->surface));
    lv_style_set_border_color(&s->checkbox, tok_color(c->outline));
    lv_style_set_border_width(&s->checkbox, 2);
    lv_style_set_radius(&s->checkbox, gui_theme_radius(GUI_THEME_RADIUS_SM));

    lv_style_init(&s->checkbox_checked);
    lv_style_set_bg_color(&s->checkbox_checked, tok_color(c->primary));
    lv_style_set_border_color(&s->checkbox_checked, tok_color(c->primary));

    /* 滑条 / 进度条。 */
    lv_style_init(&s->slider);
    lv_style_set_bg_color(&s->slider, tok_color(c->secondary_container));
    lv_style_set_bg_opa(&s->slider, LV_OPA_COVER);
    lv_style_set_radius(&s->slider, gui_theme_radius(GUI_THEME_RADIUS_FULL));

    lv_style_init(&s->slider_indicator);
    lv_style_set_bg_color(&s->slider_indicator, tok_color(c->primary));
    lv_style_set_radius(&s->slider_indicator, gui_theme_radius(GUI_THEME_RADIUS_FULL));

    lv_style_init(&s->bar);
    lv_style_set_bg_color(&s->bar, tok_color(c->secondary_container));
    lv_style_set_bg_opa(&s->bar, LV_OPA_COVER);
    lv_style_set_radius(&s->bar, gui_theme_radius(GUI_THEME_RADIUS_FULL));

    lv_style_init(&s->bar_indicator);
    lv_style_set_bg_color(&s->bar_indicator, tok_color(c->primary));
    lv_style_set_radius(&s->bar_indicator, gui_theme_radius(GUI_THEME_RADIUS_FULL));

    /* 列表。 */
    lv_style_init(&s->list);
    lv_style_set_bg_color(&s->list, tok_color(c->surface));
    lv_style_set_bg_opa(&s->list, LV_OPA_COVER);
    lv_style_set_border_width(&s->list, 0);
    lv_style_set_radius(&s->list, gui_theme_radius(GUI_THEME_RADIUS_MD));
    lv_style_set_pad_all(&s->list, gui_theme_space(GUI_THEME_SPACE_SM));

    lv_style_init(&s->list_item);
    lv_style_set_bg_color(&s->list_item, tok_color(c->surface));
    lv_style_set_bg_opa(&s->list_item, LV_OPA_COVER);
    lv_style_set_radius(&s->list_item, gui_theme_radius(GUI_THEME_RADIUS_MD));
    lv_style_set_pad_all(&s->list_item, gui_theme_space(GUI_THEME_SPACE_SM));
    lv_style_set_border_width(&s->list_item, 0);
    lv_style_set_text_color(&s->list_item, tok_color(c->on_surface));
    lv_style_set_text_font(&s->list_item, tok_font(GUI_THEME_FONT_TITLE));

    /* 滚动条。 */
    lv_style_init(&s->scrollbar);
    lv_style_set_bg_color(&s->scrollbar, tok_color(c->outline_variant));
    lv_style_set_bg_opa(&s->scrollbar, LV_OPA_COVER);
    lv_style_set_radius(&s->scrollbar, gui_theme_radius(GUI_THEME_RADIUS_FULL));
    lv_style_set_border_width(&s->scrollbar, 0);

    /* 文本输入区。 */
    lv_style_init(&s->textarea);
    lv_style_set_bg_color(&s->textarea, tok_color(c->surface_variant));
    lv_style_set_bg_opa(&s->textarea, LV_OPA_COVER);
    lv_style_set_radius(&s->textarea, gui_theme_radius(GUI_THEME_RADIUS_MD));
    lv_style_set_border_width(&s->textarea, 0);
    lv_style_set_pad_all(&s->textarea, gui_theme_space(GUI_THEME_SPACE_SM));
    lv_style_set_text_color(&s->textarea, tok_color(c->on_surface));
    lv_style_set_text_font(&s->textarea, tok_font(GUI_THEME_FONT_BODY));
}

/*
 * 释放样式块里每个样式的值缓冲。lv_style_reset() 会把 LVGL 内存池里的
 * 值数组 free 掉，因此必须在 lv_deinit() **之前**调用，否则释放的是
 * 已经失效的池。
 */
static void style_release(theme_styles_t *s)
{
    lv_style_reset(&s->scr);
    lv_style_reset(&s->card);
    lv_style_reset(&s->card_hover);
    lv_style_reset(&s->card_pressed);
    lv_style_reset(&s->label);
    lv_style_reset(&s->label_muted);
    lv_style_reset(&s->btn);
    lv_style_reset(&s->btn_hover);
    lv_style_reset(&s->btn_pressed);
    lv_style_reset(&s->btn_disabled);
    lv_style_reset(&s->btn_focused);
    lv_style_reset(&s->focus_ring);
    lv_style_reset(&s->sw);
    lv_style_reset(&s->sw_knob);
    lv_style_reset(&s->sw_checked);
    lv_style_reset(&s->sw_checked_knob);
    lv_style_reset(&s->checkbox);
    lv_style_reset(&s->checkbox_checked);
    lv_style_reset(&s->slider);
    lv_style_reset(&s->slider_indicator);
    lv_style_reset(&s->bar);
    lv_style_reset(&s->bar_indicator);
    lv_style_reset(&s->list);
    lv_style_reset(&s->list_item);
    lv_style_reset(&s->scrollbar);
    lv_style_reset(&s->textarea);
}

/* ── 主题应用 ───────────────────────────────────────────────────── */

static void theme_apply(lv_theme_t *th, lv_obj_t *obj)
{
    theme_styles_t *s = (theme_styles_t *)th->user_data;
    if (s == NULL) {
        return;
    }

    if (lv_obj_get_parent(obj) == NULL) {
        /* 屏幕本身：壁纸。 */
        lv_obj_add_style(obj, &s->scr, 0);
        lv_obj_add_style(obj, &s->scrollbar, LV_PART_SCROLLBAR);
        return;
    }

#if LV_USE_BUTTON
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s->btn, 0);
        lv_obj_add_style(obj, &s->btn_hover, LV_STATE_HOVERED);
        lv_obj_add_style(obj, &s->btn_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s->btn_disabled, LV_STATE_DISABLED);
        lv_obj_add_style(obj, &s->btn_focused, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s->btn_focused, LV_STATE_FOCUS_KEY);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUS_KEY);
        return;
    }
#endif

#if LV_USE_SWITCH
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s->sw, 0);
        lv_obj_add_style(obj, &s->sw_knob, LV_PART_KNOB);
        lv_obj_add_style(obj, &s->sw_checked, LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s->sw_checked_knob, LV_PART_KNOB | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUS_KEY);
        return;
    }
#endif

#if LV_USE_CHECKBOX
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &s->checkbox, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s->checkbox_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s->label, 0);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUS_KEY);
        return;
    }
#endif

#if LV_USE_SLIDER
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s->slider, LV_PART_MAIN);
        lv_obj_add_style(obj, &s->slider_indicator, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUS_KEY);
        return;
    }
#endif

#if LV_USE_BAR
    if (lv_obj_check_type(obj, &lv_bar_class)) {
        lv_obj_add_style(obj, &s->bar, LV_PART_MAIN);
        lv_obj_add_style(obj, &s->bar_indicator, LV_PART_INDICATOR);
        return;
    }
#endif

#if LV_USE_TEXTAREA
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s->textarea, 0);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUS_KEY);
        return;
    }
#endif

#if LV_USE_LIST
    if (lv_obj_check_type(obj, &lv_list_class)) {
        lv_obj_add_style(obj, &s->list, 0);
        lv_obj_add_style(obj, &s->scrollbar, LV_PART_SCROLLBAR);
        return;
    }
#endif

#if LV_USE_LABEL
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_add_style(obj, &s->label, 0);
        return;
    }
#endif

    /* 其余一律按容器（卡片）处理。 */
    lv_obj_add_style(obj, &s->card, 0);
    lv_obj_add_style(obj, &s->card_hover, LV_STATE_HOVERED);
    lv_obj_add_style(obj, &s->card_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUSED);
    lv_obj_add_style(obj, &s->focus_ring, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(obj, &s->scrollbar, LV_PART_SCROLLBAR);
}

lv_theme_t *gui_theme_create(lv_display_t *display)
{
    if (display == NULL) {
        return NULL;
    }

    theme_styles_t *styles = lv_calloc(1, sizeof(theme_styles_t));
    if (styles == NULL) {
        return NULL;
    }
    style_init(styles);

    lv_theme_t *theme = lv_theme_create();
    if (theme == NULL) {
        lv_free(styles);
        return NULL;
    }

    theme->user_data = styles;
    theme->disp = display;
    theme->color_primary = tok_color(gui_theme_colors()->primary);
    theme->color_secondary = tok_color(gui_theme_colors()->secondary);
    theme->font_small = tok_font(GUI_THEME_FONT_LABEL);
    theme->font_normal = tok_font(GUI_THEME_FONT_BODY);
    theme->font_large = tok_font(GUI_THEME_FONT_HEADLINE);
    lv_theme_set_apply_cb(theme, theme_apply);

    lv_display_set_theme(display, theme);

    /*
     * 显式给当前屏幕套用主题：LVGL 的 lv_display_set_theme() 只在
     * display 有 4 个 screen 时才自动重套（lv_display.c 的 screen_cnt
     * 判断），而 libgui 的 display 只有一个 screen。不显式套用的话，
     * 壁纸（surface）与状态栏底色都不会生效。
     */
    lv_obj_t *scr = lv_display_get_screen_active(display);
    if (scr != NULL) {
        lv_theme_apply(scr);
    }

    return theme;
}

void gui_theme_destroy(lv_theme_t *theme)
{
    if (theme == NULL) {
        return;
    }

    theme_styles_t *styles = (theme_styles_t *)theme->user_data;
    if (styles != NULL) {
        style_release(styles);
        lv_free(styles);
        theme->user_data = NULL;
    }

    lv_theme_delete(theme);
}
