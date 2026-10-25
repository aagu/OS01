/*
 * layout.c — 类型化的几何 / 布局 / 文本模式（GUI-2b Task 4）
 *
 * 本文件是公共描述层与后端之间的接缝：调用方说的是"我要多大、放在哪、
 * 怎么排布"，这里把它翻译成后端能懂的几何与样式。
 *
 * 两条贯穿全文件的规矩：
 *
 *   1. **无部分修改**：所有校验（参数、身份、种类、尺寸循环）都在改动
 *      任何状态之前完成。失败时槽与后端对象保持原样。
 *   2. **尺寸来源是事实**：每一轴的尺寸模式（PX/CONTENT/FILL）记在槽里。
 *      尺寸循环的判定、父子关系的推导都只认这份记录，不去猜后端的
 *      内部状态。
 */

#include <gui/gui_layout.h>
#include <gui/gui_widget.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>
#include <gui_widget_private.h>

#include <errno.h>
#include <stdint.h>

/* ── 模式 → 后端 ──────────────────────────────────────────────────── */

/**
 * @brief 把尺寸模式翻译成后端的尺寸值
 *
 * CONTENT 用后端的"按内容"常量；FILL 用 100% —— 后端把它解析成父控件
 * **内容区**的全长（父的内边距已经扣掉），正好就是"占满父内容区"。
 */
static int32_t size_to_lvgl(gui_size_mode_t mode, int32_t value)
{
    switch (mode) {
    case GUI_SIZE_MODE_PX:
        return value;
    case GUI_SIZE_MODE_CONTENT:
        return LV_SIZE_CONTENT;
    case GUI_SIZE_MODE_FILL:
        return lv_pct(100);
    default:
        return LV_SIZE_CONTENT;
    }
}

/**
 * @brief 锚点 → 后端锚点
 *
 * 只有九个合法锚点能走到这里（调用方已校验过哨兵）。
 */
static lv_align_t anchor_to_lvgl(gui_align_t anchor)
{
    switch (anchor) {
    case GUI_ALIGN_TOP_LEFT:     return LV_ALIGN_TOP_LEFT;
    case GUI_ALIGN_TOP_MID:      return LV_ALIGN_TOP_MID;
    case GUI_ALIGN_TOP_RIGHT:    return LV_ALIGN_TOP_RIGHT;
    case GUI_ALIGN_MID_LEFT:     return LV_ALIGN_LEFT_MID;
    case GUI_ALIGN_MID:          return LV_ALIGN_CENTER;
    case GUI_ALIGN_MID_RIGHT:    return LV_ALIGN_RIGHT_MID;
    case GUI_ALIGN_BOTTOM_LEFT:  return LV_ALIGN_BOTTOM_LEFT;
    case GUI_ALIGN_BOTTOM_MID:   return LV_ALIGN_BOTTOM_MID;
    case GUI_ALIGN_BOTTOM_RIGHT: return LV_ALIGN_BOTTOM_RIGHT;
    default:                     return LV_ALIGN_TOP_LEFT;
    }
}

/**
 * @brief 排布对齐 → 后端对齐
 */
static lv_flex_align_t flow_align_to_lvgl(gui_flow_align_t align)
{
    switch (align) {
    case GUI_FLOW_ALIGN_START:  return LV_FLEX_ALIGN_START;
    case GUI_FLOW_ALIGN_CENTER: return LV_FLEX_ALIGN_CENTER;
    case GUI_FLOW_ALIGN_END:    return LV_FLEX_ALIGN_END;
    default:                    return LV_FLEX_ALIGN_START;
    }
}

/* ── 尺寸循环判定 ──────────────────────────────────────────────────── */

/**
 * @brief 父控件在某一轴上是不是"按内容"
 *
 * 没有父控件（控件是根）时返回 false：根的默认尺寸来源是视口的固定
 * 像素，屏幕不是控件，不参与循环判定。
 */
static bool parent_is_content(gui_platform_t *platform, const gui_widget_slot_t *slot,
                              bool width_axis)
{
    if (!slot->parent_id) {
        return false;
    }
    gui_widget_slot_t *parent = gui_widget_slot_find(platform, slot->parent_id);
    if (!parent) {
        return false;
    }
    return width_axis ? (parent->size_mode_w == GUI_SIZE_MODE_CONTENT)
                      : (parent->size_mode_h == GUI_SIZE_MODE_CONTENT);
}

/**
 * @brief 有没有子项在某一轴上是"占满父内容区"
 *
 * 只有**直接子项**参与判定：间接子孙的尺寸依赖链是传递的，直接父子
 * 一对就足以把循环掐断。
 */
static bool has_filling_child(gui_platform_t *platform, const gui_widget_slot_t *slot,
                              bool width_axis)
{
    gui_widget_registry_t *reg = platform->registry;
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *child = &reg->slots[i];
        if (child->state != GUI_WIDGET_SLOT_LIVE || child->parent_id != slot->id) {
            continue;
        }
        if (width_axis ? (child->size_mode_w == GUI_SIZE_MODE_FILL)
                       : (child->size_mode_h == GUI_SIZE_MODE_FILL)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 把一组新的尺寸模式写进槽之前，先问一句"会不会形成尺寸循环"
 *
 * 两种方向都要问：
 *   - 新尺寸是 FILL，而父控件同轴是 CONTENT → 循环；
 *   - 新尺寸是 CONTENT，而某个直接子项同轴是 FILL → 循环。
 *
 * @return true 表示会形成循环（调用方必须拒绝这次改动）
 */
static bool would_create_size_cycle(gui_platform_t *platform, const gui_widget_slot_t *slot,
                                    gui_size_mode_t mode_w, gui_size_mode_t mode_h)
{
    if (mode_w == GUI_SIZE_MODE_FILL && parent_is_content(platform, slot, true)) {
        return true;
    }
    if (mode_h == GUI_SIZE_MODE_FILL && parent_is_content(platform, slot, false)) {
        return true;
    }
    if (mode_w == GUI_SIZE_MODE_CONTENT && has_filling_child(platform, slot, true)) {
        return true;
    }
    if (mode_h == GUI_SIZE_MODE_CONTENT && has_filling_child(platform, slot, false)) {
        return true;
    }
    return false;
}

/* ── 公共设置器 ───────────────────────────────────────────────────── */

int gui_widget_set_geometry(gui_platform_t *platform, gui_widget_t widget,
                            const gui_geometry_t *geometry)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    if (!geometry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = gui_widget_slot_find(platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)geometry->width.mode >= (unsigned int)GUI_SIZE_MODE_COUNT ||
        (unsigned int)geometry->height.mode >= (unsigned int)GUI_SIZE_MODE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    /* 尺寸循环必须在改动任何状态之前被拒绝。 */
    if (would_create_size_cycle(platform, slot, geometry->width.mode, geometry->height.mode)) {
        errno = EINVAL;
        return -1;
    }

    /* 定位切成绝对坐标；尺寸来源由调用方给出。 */
    lv_obj_set_align(slot->obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(slot->obj, geometry->x, geometry->y);
    lv_obj_set_width(slot->obj, size_to_lvgl(geometry->width.mode, geometry->width.value));
    lv_obj_set_height(slot->obj, size_to_lvgl(geometry->height.mode, geometry->height.value));

    slot->absolute_pos = true;
    slot->size_mode_w = geometry->width.mode;
    slot->size_mode_h = geometry->height.mode;
    slot->size_value_w = geometry->width.value;
    slot->size_value_h = geometry->height.value;
    return 0;
}

int gui_widget_set_alignment(gui_platform_t *platform, gui_widget_t widget,
                             const gui_alignment_t *alignment)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    if (!alignment) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = gui_widget_slot_find(platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)alignment->anchor >= (unsigned int)GUI_ALIGN_COUNT) {
        errno = EINVAL;
        return -1;
    }

    /* 锚点定位：偏移相对锚点计算。尺寸来源不动。 */
    lv_obj_align(slot->obj, anchor_to_lvgl(alignment->anchor), alignment->x_offset,
                 alignment->y_offset);

    slot->absolute_pos = false;
    slot->anchor = alignment->anchor;
    slot->align_x = alignment->x_offset;
    slot->align_y = alignment->y_offset;
    return 0;
}

int gui_widget_set_layout(gui_platform_t *platform, gui_widget_t widget, const gui_layout_t *layout)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    if (!layout) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = gui_widget_slot_find(platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    /* 业务回调里不允许改布局：回调期间树结构必须保持稳定。 */
    if (platform->registry->callback_depth > 0) {
        errno = EBUSY;
        return -1;
    }
    /* 标签是叶子：排布对它没有意义。 */
    if (slot->kind == GUI_WIDGET_LABEL) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if ((unsigned int)layout->flow >= (unsigned int)GUI_FLOW_COUNT ||
        (unsigned int)layout->main_align >= (unsigned int)GUI_FLOW_ALIGN_COUNT ||
        (unsigned int)layout->cross_align >= (unsigned int)GUI_FLOW_ALIGN_COUNT) {
        errno = EINVAL;
        return -1;
    }
    if (layout->grow > 255u) {
        errno = EINVAL;
        return -1;
    }

    switch (layout->flow) {
    case GUI_FLOW_ROW:
        lv_obj_set_flex_flow(slot->obj, LV_FLEX_FLOW_ROW);
        /* 横向排布时，相邻子项之间的间距落在"列间距"上。 */
        lv_obj_set_style_pad_column(slot->obj, layout->gap, 0);
        lv_obj_set_flex_align(slot->obj, flow_align_to_lvgl(layout->main_align),
                              flow_align_to_lvgl(layout->cross_align), LV_FLEX_ALIGN_START);
        break;
    case GUI_FLOW_COLUMN:
        lv_obj_set_flex_flow(slot->obj, LV_FLEX_FLOW_COLUMN);
        /* 纵向排布时，相邻子项之间的间距落在"行间距"上。 */
        lv_obj_set_style_pad_row(slot->obj, layout->gap, 0);
        lv_obj_set_flex_align(slot->obj, flow_align_to_lvgl(layout->main_align),
                              flow_align_to_lvgl(layout->cross_align), LV_FLEX_ALIGN_START);
        break;
    case GUI_FLOW_NONE:
    default:
        /* 不排布：子项各自定位。
         *
         * 注意：后端的 flex 对齐入口会顺手把排布模式打开，所以"不排布"
         * 这一支**不能**调用它——否则排布又被偷偷打开了。 */
        lv_obj_set_style_layout(slot->obj, LV_LAYOUT_NONE, 0);
        break;
    }

    lv_obj_set_flex_grow(slot->obj, (uint8_t)layout->grow);

    lv_obj_set_style_pad_top(slot->obj, layout->pad_top, 0);
    lv_obj_set_style_pad_bottom(slot->obj, layout->pad_bottom, 0);
    lv_obj_set_style_pad_left(slot->obj, layout->pad_left, 0);
    lv_obj_set_style_pad_right(slot->obj, layout->pad_right, 0);

    slot->flow = layout->flow;
    slot->main_align = layout->main_align;
    slot->cross_align = layout->cross_align;
    slot->grow = layout->grow;
    slot->gap = layout->gap;
    slot->pad_top = layout->pad_top;
    slot->pad_bottom = layout->pad_bottom;
    slot->pad_left = layout->pad_left;
    slot->pad_right = layout->pad_right;
    return 0;
}

int gui_widget_set_scroll(gui_platform_t *platform, gui_widget_t widget, const gui_scroll_t *scroll)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    if (!scroll) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = gui_widget_slot_find(platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)scroll->text_mode >= (unsigned int)GUI_TEXT_MODE_COUNT ||
        (unsigned int)scroll->text_align >= (unsigned int)GUI_TEXT_ALIGN_COUNT) {
        errno = EINVAL;
        return -1;
    }

    if (slot->kind == GUI_WIDGET_LABEL) {
        /* 标签不滚动：滚动标志必须都是 false。 */
        if (scroll->horizontal || scroll->vertical) {
            errno = EINVAL;
            return -1;
        }
    } else {
        /* 容器与按钮不承载长文本：文本字段必须是默认值。 */
        if (scroll->text_mode != GUI_TEXT_MODE_WRAP ||
            scroll->text_align != GUI_TEXT_ALIGN_LEFT) {
            errno = EINVAL;
            return -1;
        }
    }

    lv_dir_t dir = LV_DIR_NONE;
    if (scroll->horizontal && scroll->vertical) {
        dir = LV_DIR_ALL;
    } else if (scroll->horizontal) {
        dir = LV_DIR_HOR;
    } else if (scroll->vertical) {
        dir = LV_DIR_VER;
    }
    lv_obj_set_scroll_dir(slot->obj, dir);
    lv_obj_set_scrollbar_mode(slot->obj,
                              dir == LV_DIR_NONE ? LV_SCROLLBAR_MODE_OFF : LV_SCROLLBAR_MODE_AUTO);

    if (slot->kind == GUI_WIDGET_LABEL) {
        lv_label_set_long_mode(slot->obj, scroll->text_mode == GUI_TEXT_MODE_CLIP
                                                ? LV_LABEL_LONG_CLIP
                                                : LV_LABEL_LONG_WRAP);
    }
    lv_obj_set_style_text_align(slot->obj,
                                 scroll->text_align == GUI_TEXT_ALIGN_CENTER
                                     ? LV_TEXT_ALIGN_CENTER
                                     : (scroll->text_align == GUI_TEXT_ALIGN_RIGHT
                                            ? LV_TEXT_ALIGN_RIGHT
                                            : LV_TEXT_ALIGN_LEFT),
                                 0);

    slot->scroll_h = scroll->horizontal;
    slot->scroll_v = scroll->vertical;
    slot->text_mode = scroll->text_mode;
    slot->text_align = scroll->text_align;
    return 0;
}
