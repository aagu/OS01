/*
 * appearance.c — 用 Material 角色表达原生外观（GUI-2b Task 5）
 *
 * 本文件是"外观描述"与"主题令牌"之间的接缝：调用方说的是"底色用哪个
 * 角色、要不要圆角、边框画不画"，这里把它翻译成样式。
 *
 * 两条贯穿全文件的规矩：
 *
 *   1. **无部分修改**：所有校验（参数、身份、种类与形态的相容性、角色与
 *      令牌的合法性）都在改动任何状态之前完成。
 *   2. **只改普通状态**：所有样式都写在**普通状态**（selector 0）上。
 *      主题的 hover / pressed / focused / disabled 样式原样保留——尤其
 *      是 FOCUSED 的焦点环：它是 2px 的 primary 轮廓，普通局部边框遮不
 *      掉它。
 */

#include <gui/gui_layout.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>
#include <gui_widget_private.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

/* ── 角色 → 令牌 ──────────────────────────────────────────────────── */

/**
 * @brief 色彩角色 → 令牌表里的颜色
 *
 * 角色与令牌表的字段一一对应：枚举的顺序就是字段的顺序，因此这里的
 * switch 就是"枚举覆盖每一字段"的落地。
 */
static uint32_t color_role_rgb(gui_color_role_t role)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    switch (role) {
    case GUI_COLOR_PRIMARY:                return c->primary;
    case GUI_COLOR_ON_PRIMARY:             return c->on_primary;
    case GUI_COLOR_PRIMARY_CONTAINER:      return c->primary_container;
    case GUI_COLOR_ON_PRIMARY_CONTAINER:   return c->on_primary_container;
    case GUI_COLOR_SECONDARY:              return c->secondary;
    case GUI_COLOR_ON_SECONDARY:           return c->on_secondary;
    case GUI_COLOR_SECONDARY_CONTAINER:    return c->secondary_container;
    case GUI_COLOR_ON_SECONDARY_CONTAINER: return c->on_secondary_container;
    case GUI_COLOR_TERTIARY:               return c->tertiary;
    case GUI_COLOR_ON_TERTIARY:            return c->on_tertiary;
    case GUI_COLOR_TERTIARY_CONTAINER:     return c->tertiary_container;
    case GUI_COLOR_ON_TERTIARY_CONTAINER:  return c->on_tertiary_container;
    case GUI_COLOR_ERROR:                  return c->error;
    case GUI_COLOR_ON_ERROR:               return c->on_error;
    case GUI_COLOR_ERROR_CONTAINER:        return c->error_container;
    case GUI_COLOR_ON_ERROR_CONTAINER:     return c->on_error_container;
    case GUI_COLOR_SUCCESS:                return c->success;
    case GUI_COLOR_ON_SUCCESS:             return c->on_success;
    case GUI_COLOR_SUCCESS_CONTAINER:      return c->success_container;
    case GUI_COLOR_ON_SUCCESS_CONTAINER:   return c->on_success_container;
    case GUI_COLOR_SURFACE:                return c->surface;
    case GUI_COLOR_SURFACE_DIM:            return c->surface_dim;
    case GUI_COLOR_ON_SURFACE:             return c->on_surface;
    case GUI_COLOR_SURFACE_VARIANT:        return c->surface_variant;
    case GUI_COLOR_ON_SURFACE_VARIANT:     return c->on_surface_variant;
    case GUI_COLOR_SURFACE_CONTAINER_LOW:  return c->surface_container_low;
    case GUI_COLOR_SURFACE_CONTAINER:      return c->surface_container;
    case GUI_COLOR_SURFACE_CONTAINER_HIGH: return c->surface_container_high;
    case GUI_COLOR_SURFACE_CONTAINER_HIGHEST: return c->surface_container_highest;
    case GUI_COLOR_OUTLINE:                return c->outline;
    case GUI_COLOR_OUTLINE_VARIANT:        return c->outline_variant;
    case GUI_COLOR_INVERSE_SURFACE:        return c->inverse_surface;
    case GUI_COLOR_INVERSE_ON_SURFACE:     return c->inverse_on_surface;
    case GUI_COLOR_SCRIM:                  return c->scrim;
    case GUI_COLOR_SHADOW:                 return c->shadow;
    case GUI_COLOR_COUNT:
    default:
        return c->surface;
    }
}

/**
 * @brief 字号层级 → 后端字体
 */
static const lv_font_t *font_of(gui_theme_font_t role)
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

/* ── 形态与种类的相容性 ────────────────────────────────────────────── */

/**
 * @brief 这个控件种类能不能用这个形态
 *
 * `is_root` 指控件没有控件父级（平台根容器）。
 */
static bool variant_allowed(gui_widget_kind_t kind, gui_variant_t variant, bool is_root)
{
    switch (kind) {
    case GUI_WIDGET_LABEL:
        /* 标签是叶子：只有素面。 */
        return variant == GUI_VARIANT_PLAIN;
    case GUI_WIDGET_CONTAINER:
        if (variant == GUI_VARIANT_ROOT) {
            return is_root;
        }
        return variant == GUI_VARIANT_PANEL || variant == GUI_VARIANT_CARD ||
               variant == GUI_VARIANT_PLAIN;
    case GUI_WIDGET_BUTTON:
        return variant == GUI_VARIANT_ACTION || variant == GUI_VARIANT_CARD ||
               variant == GUI_VARIANT_PLAIN;
    default:
        return false;
    }
}

/* ── 落地 ──────────────────────────────────────────────────────────── */

/**
 * @brief 把外观写进后端对象
 *
 * 全部写在普通状态（selector 0）上：主题的各状态样式因此原样生效。
 */
static void appearance_apply(lv_obj_t *obj, const gui_appearance_t *ap)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color_role_rgb(ap->bg_role)), 0);
    lv_obj_set_style_bg_opa(obj, ap->filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    lv_obj_set_style_text_color(obj, lv_color_hex(color_role_rgb(ap->text_role)), 0);
    lv_obj_set_style_text_font(obj, font_of(ap->font), 0);

    lv_obj_set_style_radius(obj, ap->rounded ? gui_theme_radius(ap->radius) : 0, 0);

    lv_obj_set_style_border_width(obj, ap->bordered ? 1 : 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color_role_rgb(ap->border_role)), 0);
    lv_obj_set_style_border_opa(obj, ap->bordered ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    lv_obj_set_style_shadow_color(obj, lv_color_hex(color_role_rgb(GUI_COLOR_SHADOW)), 0);
    lv_obj_set_style_shadow_width(obj, gui_theme_elevation_shadow_width(ap->elevation), 0);
    lv_obj_set_style_shadow_offset_y(obj, gui_theme_elevation_shadow_offset_y(ap->elevation), 0);
    lv_obj_set_style_shadow_opa(obj, gui_theme_elevation_shadow_opa(ap->elevation), 0);

    if (ap->variant == GUI_VARIANT_ROOT) {
        /* 根容器是全屏底板：它不留内边距，底板直接贴边。 */
        lv_obj_set_style_pad_top(obj, 0, 0);
        lv_obj_set_style_pad_bottom(obj, 0, 0);
        lv_obj_set_style_pad_left(obj, 0, 0);
        lv_obj_set_style_pad_right(obj, 0, 0);
    }
}

/* ── 公共设置器 ───────────────────────────────────────────────────── */

int gui_widget_set_appearance(gui_platform_t *platform, gui_widget_t widget,
                              const gui_appearance_t *appearance)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    if (!appearance) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = gui_widget_slot_find(platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }

    /* 角色越界。 */
    if ((unsigned int)appearance->bg_role >= (unsigned int)GUI_COLOR_COUNT ||
        (unsigned int)appearance->text_role >= (unsigned int)GUI_COLOR_COUNT ||
        (unsigned int)appearance->border_role >= (unsigned int)GUI_COLOR_COUNT) {
        errno = EINVAL;
        return -1;
    }
    /* 形态越界。 */
    if ((unsigned int)appearance->variant >= (unsigned int)GUI_VARIANT_COUNT) {
        errno = EINVAL;
        return -1;
    }
    /* 令牌层级越界：令牌表自己对非法层级返回 -1。 */
    if (gui_theme_font_size(appearance->font) < 0 ||
        gui_theme_radius(appearance->radius) < 0 ||
        gui_theme_elevation_shadow_width(appearance->elevation) < 0) {
        errno = EINVAL;
        return -1;
    }

    /* 形态与种类必须相容。 */
    bool is_root = (slot->parent_id == 0);
    if (!variant_allowed(slot->kind, appearance->variant, is_root)) {
        errno = EINVAL;
        return -1;
    }

    /* 根底板必须是透明的、无边框、无圆角、无 elevation 的。 */
    if (appearance->variant == GUI_VARIANT_ROOT) {
        if (appearance->filled || appearance->bordered || appearance->rounded ||
            appearance->elevation != GUI_THEME_ELEVATION_0) {
            errno = EINVAL;
            return -1;
        }
    }

    appearance_apply(slot->obj, appearance);
    slot->appearance = *appearance;
    return 0;
}

/* ── 创建时的默认外观 ──────────────────────────────────────────────── */

void gui_widget_appearance_init(gui_widget_kind_t kind, bool is_root, gui_appearance_t *out)
{
    if (!out) {
        return;
    }

    *out = (gui_appearance_t)GUI_APPEARANCE_DEFAULT;

    switch (kind) {
    case GUI_WIDGET_LABEL:
        /* 标签不画底：它只显示文字。 */
        out->variant = GUI_VARIANT_PLAIN;
        out->filled = false;
        break;
    case GUI_WIDGET_BUTTON:
        /* 动作形态取主题里按钮的现有默认：primary 底 + on_primary 文字 +
         * 中圆角 + 标题字号。 */
        out->variant = GUI_VARIANT_ACTION;
        out->bg_role = GUI_COLOR_PRIMARY;
        out->text_role = GUI_COLOR_ON_PRIMARY;
        out->border_role = GUI_COLOR_OUTLINE;
        out->font = GUI_THEME_FONT_TITLE;
        out->radius = GUI_THEME_RADIUS_MD;
        out->elevation = GUI_THEME_ELEVATION_0;
        out->filled = true;
        out->rounded = true;
        out->bordered = false;
        break;
    case GUI_WIDGET_CONTAINER:
    default:
        if (is_root) {
            /* 根是全屏底板：透明、无边框、无圆角、无 elevation。 */
            out->variant = GUI_VARIANT_ROOT;
            out->filled = false;
            out->rounded = false;
            out->bordered = false;
            out->elevation = GUI_THEME_ELEVATION_0;
        }
        break;
    }
}

void gui_widget_appearance_apply(lv_obj_t *obj, const gui_appearance_t *appearance)
{
    if (!obj || !appearance) {
        return;
    }
    appearance_apply(obj, appearance);
}
