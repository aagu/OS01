/*
 * test_gui_widget_layout.c — 类型化几何 / 布局 / 文本模式（GUI-2b Task 4）
 *
 * 本文件是**公共契约**测试：它只包含 gui 目录下的公共头、标准库、测试
 * 框架和宿主私有 fixture 头。断言全部通过公共 API 与值句柄表达。
 *
 * 断言策略：主题给容器留的内边距与边框宽度是**实现细节**（GUI-3 的令牌
 * 表将来会改），因此这里不去硬编码它们，而是把内容区原点与内容尺寸
 * **量**出来，再对关系（差值、跟随、相等）下断言。
 */

#include "test_framework.h"

#include <gui/gui.h>
#include <gui/gui_layout.h>
#include <gui/gui_widget.h>

#include "gui_native_fixture.h"
#include "gui_test_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* The platform TU's host-only header tamper switch.  Every GUI host
 * fixture defines it; this fixture never flips it. */
bool g_gui_test_tamper_header = false;

/* ── 测量工具 ────────────────────────────────────────────────────── */

/**
 * @brief 量出父控件的内容区原点与内容尺寸
 *
 * 用一个 (0,0) 的探针子项把内容区原点量出来；内容区尺寸由父控件的总
 * 尺寸减去两侧的"边框 + 内边距"得到（主题的边框与内边距在四边对称）。
 */
static void measure_content_box(gui_platform_t *platform, gui_widget_t parent, int32_t *out_x,
                                int32_t *out_y, int32_t *out_w, int32_t *out_h)
{
    gui_widget_t probe;
    assert_eq(0, gui_widget_create(platform, parent, GUI_WIDGET_CONTAINER, &probe));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(1);
    g.height = GUI_SIZE_PX(1);
    assert_eq(0, gui_widget_set_geometry(platform, probe, &g));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(platform, probe, &r));
    if (out_x) {
        *out_x = r.x;
    }
    if (out_y) {
        *out_y = r.y;
    }

    gui_rect_t pr;
    assert_eq(0, gui_widget_get_bounds(platform, parent, &pr));
    int32_t chrome_x = r.x - pr.x;
    int32_t chrome_y = r.y - pr.y;
    if (out_w) {
        *out_w = pr.width - 2 * chrome_x;
    }
    if (out_h) {
        *out_h = pr.height - 2 * chrome_y;
    }

    assert_eq(0, gui_widget_destroy(platform, probe));
}

/**
 * @brief 容器内容区的宽度
 *
 * 从纵轴量出"边框 + 上内边距"（横纵两侧用的是同一组刻度），再从总
 * 宽度里扣掉两侧。
 */
static int32_t content_width_from(gui_platform_t *platform, gui_widget_t container,
                                   int32_t first_item_y)
{
    gui_rect_t pr;
    assert_eq(0, gui_widget_get_bounds(platform, container, &pr));
    int32_t chrome = first_item_y - pr.y;
    return pr.width - 2 * chrome;
}

/* ── 类型契约 ────────────────────────────────────────────────────── */

/* 模式枚举必须能当 case 常量用。 */
static int size_mode_rank(gui_size_mode_t mode)
{
    switch (mode) {
    case GUI_SIZE_MODE_PX:
        return 1;
    case GUI_SIZE_MODE_CONTENT:
        return 2;
    case GUI_SIZE_MODE_FILL:
        return 3;
    case GUI_SIZE_MODE_COUNT:
        return 4;
    }
    return 0;
}

TEST_FUNC(test_layout_public_types_compile)
{
    /* 哨兵不得省略：越界的参数必须能被公共 API 拒绝。 */
    assert_eq(3, GUI_SIZE_MODE_COUNT);
    assert_eq(9, GUI_ALIGN_COUNT);
    assert_eq(3, GUI_FLOW_COUNT);
    assert_eq(3, GUI_FLOW_ALIGN_COUNT);
    assert_eq(2, GUI_TEXT_MODE_COUNT);
    assert_eq(3, GUI_TEXT_ALIGN_COUNT);

    /* 模式枚举可以当 case 常量。 */
    assert_eq(1, size_mode_rank(GUI_SIZE_MODE_PX));
    assert_eq(2, size_mode_rank(GUI_SIZE_MODE_CONTENT));
    assert_eq(3, size_mode_rank(GUI_SIZE_MODE_FILL));
    assert_eq(4, size_mode_rank(GUI_SIZE_MODE_COUNT));

    /* 构造表达式产出 gui_size_t。 */
    gui_size_t px = GUI_SIZE_PX(120);
    assert_eq(GUI_SIZE_MODE_PX, px.mode);
    assert_eq(120, px.value);

    gui_size_t content = GUI_SIZE_CONTENT;
    assert_eq(GUI_SIZE_MODE_CONTENT, content.mode);
    assert_eq(0, content.value);

    gui_size_t fill = GUI_SIZE_FILL;
    assert_eq(GUI_SIZE_MODE_FILL, fill.mode);
    assert_eq(0, fill.value);

    /* 两个命名空间不冲突：模式常量与构造表达式落在结构体的不同字段上，
     * 谁也不会把谁盖掉。 */
    gui_size_t built = { GUI_SIZE_MODE_FILL, 7 };
    assert_eq(GUI_SIZE_MODE_FILL, built.mode);
    assert_eq(7, built.value);

    /* 几何是对齐与布局的公共描述：都是可聚合的值类型。 */
    gui_geometry_t geometry = { 1, 2, GUI_SIZE_PX(3), GUI_SIZE_CONTENT };
    assert_eq(1, geometry.x);
    assert_eq(2, geometry.y);
    assert_eq(GUI_SIZE_MODE_PX, geometry.width.mode);
    assert_eq(GUI_SIZE_MODE_CONTENT, geometry.height.mode);

    gui_alignment_t alignment = { GUI_ALIGN_MID, 4, 5 };
    assert_eq(GUI_ALIGN_MID, alignment.anchor);
    assert_eq(4, alignment.x_offset);
    assert_eq(5, alignment.y_offset);

    gui_layout_t layout = { GUI_FLOW_ROW, GUI_FLOW_ALIGN_CENTER, GUI_FLOW_ALIGN_END, 2, 6, 1, 2, 3, 4 };
    assert_eq(GUI_FLOW_ROW, layout.flow);
    assert_eq(GUI_FLOW_ALIGN_CENTER, layout.main_align);
    assert_eq(GUI_FLOW_ALIGN_END, layout.cross_align);
    assert_eq(2, layout.grow);
    assert_eq(6, layout.gap);
    assert_eq(1, layout.pad_top);
    assert_eq(2, layout.pad_bottom);
    assert_eq(3, layout.pad_left);
    assert_eq(4, layout.pad_right);

    gui_scroll_t scroll = { true, false, GUI_TEXT_MODE_CLIP, GUI_TEXT_ALIGN_RIGHT };
    assert_true(scroll.horizontal);
    assert_false(scroll.vertical);
    assert_eq(GUI_TEXT_MODE_CLIP, scroll.text_mode);
    assert_eq(GUI_TEXT_ALIGN_RIGHT, scroll.text_align);
}

/* ── 几何与对齐 ──────────────────────────────────────────────────── */

TEST_FUNC(test_geometry_sets_absolute_position_and_size)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &child));

    int32_t ox = 0;
    int32_t oy = 0;
    measure_content_box(fx.platform, root, &ox, &oy, NULL, NULL);

    gui_geometry_t g;
    g.x = 10;
    g.y = 20;
    g.width = GUI_SIZE_PX(100);
    g.height = GUI_SIZE_PX(50);
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &g));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(100, r.width);
    assert_eq(50, r.height);
    /* 位置是**绝对**的：相对父控件的内容区原点。 */
    assert_eq(ox + 10, r.x);
    assert_eq(oy + 20, r.y);

    /* 改位置不改尺寸。 */
    g.x = 30;
    g.y = 40;
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &g));
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(ox + 30, r.x);
    assert_eq(oy + 40, r.y);
    assert_eq(100, r.width);
    assert_eq(50, r.height);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_alignment_anchors_with_offsets)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &child));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(100);
    g.height = GUI_SIZE_PX(50);
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &g));

    int32_t ox = 0;
    int32_t oy = 0;
    int32_t ow = 0;
    int32_t oh = 0;
    measure_content_box(fx.platform, root, &ox, &oy, &ow, &oh);

    gui_alignment_t a;
    a.anchor = GUI_ALIGN_MID;
    a.x_offset = 5;
    a.y_offset = -3;
    assert_eq(0, gui_widget_set_alignment(fx.platform, child, &a));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    /* 锚点居中 + 偏移：偏移相对锚点计算。 */
    assert_eq(ox + (ow / 2) - (100 / 2) + 5, r.x);
    assert_eq(oy + (-3) + (oh / 2) - (50 / 2), r.y);
    /* 对齐不碰尺寸。 */
    assert_eq(100, r.width);
    assert_eq(50, r.height);

    /* 换成左上角锚点 + 偏移。 */
    a.anchor = GUI_ALIGN_TOP_LEFT;
    a.x_offset = 7;
    a.y_offset = 9;
    assert_eq(0, gui_widget_set_alignment(fx.platform, child, &a));
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(ox + 7, r.x);
    assert_eq(oy + 9, r.y);
    assert_eq(100, r.width);
    assert_eq(50, r.height);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_geometry_and_alignment_keep_size_source)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &child));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(100);
    g.height = GUI_SIZE_PX(50);
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &g));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(100, r.width);

    /* 切到锚点定位：尺寸来源不被抹掉。 */
    gui_alignment_t a = { GUI_ALIGN_TOP_LEFT, 0, 0 };
    assert_eq(0, gui_widget_set_alignment(fx.platform, child, &a));
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(100, r.width);
    assert_eq(50, r.height);

    /* 切回绝对定位：尺寸来源同样不被抹掉。 */
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &g));
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(100, r.width);
    assert_eq(50, r.height);

    gui_native_fixture_destroy(&fx);
}

/* ── FILL 与尺寸循环 ─────────────────────────────────────────────── */

TEST_FUNC(test_fill_tracks_parent_content_length)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &child));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_FILL;
    g.height = GUI_SIZE_PX(10);
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &g));

    int32_t ox = 0;
    int32_t oy = 0;
    int32_t ow = 0;
    measure_content_box(fx.platform, root, &ox, &oy, &ow, NULL);

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    /* FILL = 父控件内容区的全长（父的内边距已经扣掉）。 */
    assert_eq(ow, r.width);
    assert_true(r.width < 640);

    /* 父控件变窄：FILL 子项跟着变窄，差值正好是父控件的宽度差。 */
    gui_geometry_t root_geo;
    root_geo.x = 0;
    root_geo.y = 0;
    root_geo.width = GUI_SIZE_PX(400);
    root_geo.height = GUI_SIZE_PX(300);
    assert_eq(0, gui_widget_set_geometry(fx.platform, root, &root_geo));

    int32_t ow2 = 0;
    measure_content_box(fx.platform, root, NULL, NULL, &ow2, NULL);
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(ow2, r.width);
    assert_eq(ow - ow2, 240);

    /* 父控件的内边距变宽：内容区变窄，FILL 子项跟着变窄。 */
    gui_layout_t lay;
    lay.flow = GUI_FLOW_NONE;
    lay.main_align = GUI_FLOW_ALIGN_START;
    lay.cross_align = GUI_FLOW_ALIGN_START;
    lay.grow = 0;
    lay.gap = 0;
    lay.pad_top = 0;
    lay.pad_bottom = 0;
    lay.pad_left = 20;
    lay.pad_right = 20;
    assert_eq(0, gui_widget_set_layout(fx.platform, root, &lay));

    int32_t ow3 = 0;
    measure_content_box(fx.platform, root, NULL, NULL, &ow3, NULL);
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &r));
    assert_eq(ow3, r.width);
    assert_true(ow3 < ow2);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_content_parent_with_fill_child_is_rejected)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* 方向一：父按内容 + 子占满父 → 循环。 */
    gui_widget_t parent;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &parent));
    gui_geometry_t parent_geo;
    parent_geo.x = 0;
    parent_geo.y = 0;
    parent_geo.width = GUI_SIZE_CONTENT;
    parent_geo.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, parent, &parent_geo));

    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, parent, GUI_WIDGET_CONTAINER, &child));
    gui_geometry_t child_geo;
    child_geo.x = 5;
    child_geo.y = 6;
    child_geo.width = GUI_SIZE_PX(20);
    child_geo.height = GUI_SIZE_PX(10);
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &child_geo));

    gui_rect_t before;
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &before));

    gui_geometry_t bad;
    bad.x = 0;
    bad.y = 0;
    bad.width = GUI_SIZE_FILL;
    bad.height = GUI_SIZE_PX(77);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, child, &bad));
    assert_eq(EINVAL, errno);

    /* 无部分修改：被拒绝的调用没有改动任何状态。 */
    gui_rect_t after;
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &after));
    assert_eq(before.x, after.x);
    assert_eq(before.y, after.y);
    assert_eq(before.width, after.width);
    assert_eq(before.height, after.height);

    /* 方向二：先占满父，再把父改成按内容 → 同样拒绝。 */
    gui_widget_t parent2;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &parent2));
    gui_geometry_t parent2_geo;
    parent2_geo.x = 0;
    parent2_geo.y = 0;
    parent2_geo.width = GUI_SIZE_PX(120);
    parent2_geo.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, parent2, &parent2_geo));

    gui_widget_t child2;
    assert_eq(0, gui_widget_create(fx.platform, parent2, GUI_WIDGET_CONTAINER, &child2));
    gui_geometry_t child2_geo;
    child2_geo.x = 0;
    child2_geo.y = 0;
    child2_geo.width = GUI_SIZE_FILL;
    child2_geo.height = GUI_SIZE_PX(10);
    assert_eq(0, gui_widget_set_geometry(fx.platform, child2, &child2_geo));

    gui_rect_t parent2_before;
    assert_eq(0, gui_widget_get_bounds(fx.platform, parent2, &parent2_before));

    gui_geometry_t bad2;
    bad2.x = 0;
    bad2.y = 0;
    bad2.width = GUI_SIZE_CONTENT;
    bad2.height = GUI_SIZE_PX(60);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, parent2, &bad2));
    assert_eq(EINVAL, errno);

    gui_rect_t parent2_after;
    assert_eq(0, gui_widget_get_bounds(fx.platform, parent2, &parent2_after));
    assert_eq(parent2_before.width, parent2_after.width);
    assert_eq(parent2_before.height, parent2_after.height);

    gui_native_fixture_destroy(&fx);
}

/* ── 排布 ────────────────────────────────────────────────────────── */

TEST_FUNC(test_flow_row_and_column_place_children)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* ── 横向排布 ── */
    gui_widget_t row;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &row));
    gui_geometry_t row_geo;
    row_geo.x = 0;
    row_geo.y = 0;
    row_geo.width = GUI_SIZE_PX(300);
    row_geo.height = GUI_SIZE_PX(200);
    assert_eq(0, gui_widget_set_geometry(fx.platform, row, &row_geo));

    gui_layout_t lay;
    lay.flow = GUI_FLOW_ROW;
    lay.main_align = GUI_FLOW_ALIGN_START;
    lay.cross_align = GUI_FLOW_ALIGN_START;
    lay.grow = 0;
    lay.gap = 10;
    lay.pad_top = 0;
    lay.pad_bottom = 0;
    lay.pad_left = 0;
    lay.pad_right = 0;
    assert_eq(0, gui_widget_set_layout(fx.platform, row, &lay));

    gui_widget_t a;
    assert_eq(0, gui_widget_create(fx.platform, row, GUI_WIDGET_CONTAINER, &a));
    gui_widget_t b;
    assert_eq(0, gui_widget_create(fx.platform, row, GUI_WIDGET_CONTAINER, &b));

    gui_geometry_t item;
    item.x = 0;
    item.y = 0;
    item.width = GUI_SIZE_PX(40);
    item.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, a, &item));
    item.width = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, b, &item));

    gui_rect_t ra;
    gui_rect_t rb;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &ra));
    assert_eq(0, gui_widget_get_bounds(fx.platform, b, &rb));
    /* 横向：第二个子项在第一个之后，中间隔着 gap。 */
    assert_eq(ra.x + 40 + 10, rb.x);
    assert_eq(ra.y, rb.y);
    assert_eq(40, ra.width);
    assert_eq(60, rb.width);

    /* ── 纵向排布 ── */
    gui_widget_t column;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &column));
    row_geo.y = 220;
    assert_eq(0, gui_widget_set_geometry(fx.platform, column, &row_geo));
    lay.flow = GUI_FLOW_COLUMN;
    assert_eq(0, gui_widget_set_layout(fx.platform, column, &lay));

    gui_widget_t c;
    assert_eq(0, gui_widget_create(fx.platform, column, GUI_WIDGET_CONTAINER, &c));
    gui_widget_t d;
    assert_eq(0, gui_widget_create(fx.platform, column, GUI_WIDGET_CONTAINER, &d));
    assert_eq(0, gui_widget_set_geometry(fx.platform, c, &item));
    item.width = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, d, &item));

    gui_rect_t rc;
    gui_rect_t rd;
    assert_eq(0, gui_widget_get_bounds(fx.platform, c, &rc));
    assert_eq(0, gui_widget_get_bounds(fx.platform, d, &rd));
    /* 纵向：第二个子项在第一个之下，中间隔着 gap。 */
    assert_eq(rc.y + 20 + 10, rd.y);
    assert_eq(rc.x, rd.x);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_flow_main_alignment_centers_group)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t row;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &row));

    gui_geometry_t row_geo;
    row_geo.x = 0;
    row_geo.y = 0;
    row_geo.width = GUI_SIZE_PX(300);
    row_geo.height = GUI_SIZE_PX(200);
    assert_eq(0, gui_widget_set_geometry(fx.platform, row, &row_geo));

    gui_layout_t lay;
    lay.flow = GUI_FLOW_ROW;
    lay.main_align = GUI_FLOW_ALIGN_START;
    lay.cross_align = GUI_FLOW_ALIGN_START;
    lay.grow = 0;
    lay.gap = 10;
    lay.pad_top = 0;
    lay.pad_bottom = 0;
    lay.pad_left = 0;
    lay.pad_right = 0;
    assert_eq(0, gui_widget_set_layout(fx.platform, row, &lay));

    gui_widget_t a;
    assert_eq(0, gui_widget_create(fx.platform, row, GUI_WIDGET_CONTAINER, &a));
    gui_widget_t b;
    assert_eq(0, gui_widget_create(fx.platform, row, GUI_WIDGET_CONTAINER, &b));

    gui_geometry_t item;
    item.x = 0;
    item.y = 0;
    item.width = GUI_SIZE_PX(40);
    item.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, a, &item));
    item.width = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, b, &item));

    gui_rect_t start_a;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &start_a));
    int32_t content_w = content_width_from(fx.platform, row, start_a.y);

    /* 主轴居中：整组（40 + gap 10 + 60）居中摆放。 */
    lay.main_align = GUI_FLOW_ALIGN_CENTER;
    assert_eq(0, gui_widget_set_layout(fx.platform, row, &lay));

    gui_rect_t center_a;
    gui_rect_t center_b;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &center_a));
    assert_eq(0, gui_widget_get_bounds(fx.platform, b, &center_b));
    assert_eq(start_a.x + (content_w - 110) / 2, center_a.x);
    assert_eq(center_a.x + 40 + 10, center_b.x);

    /* 主轴靠尾：整组贴住终点。 */
    lay.main_align = GUI_FLOW_ALIGN_END;
    assert_eq(0, gui_widget_set_layout(fx.platform, row, &lay));

    gui_rect_t end_a;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &end_a));
    assert_eq(start_a.x + (content_w - 110), end_a.x);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_flow_item_size_source_is_kept)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t row;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &row));

    gui_geometry_t row_geo;
    row_geo.x = 0;
    row_geo.y = 0;
    row_geo.width = GUI_SIZE_PX(300);
    row_geo.height = GUI_SIZE_PX(200);
    assert_eq(0, gui_widget_set_geometry(fx.platform, row, &row_geo));

    gui_layout_t lay;
    lay.flow = GUI_FLOW_ROW;
    lay.main_align = GUI_FLOW_ALIGN_START;
    lay.cross_align = GUI_FLOW_ALIGN_START;
    lay.grow = 0;
    lay.gap = 0;
    lay.pad_top = 0;
    lay.pad_bottom = 0;
    lay.pad_left = 0;
    lay.pad_right = 0;
    assert_eq(0, gui_widget_set_layout(fx.platform, row, &lay));

    gui_widget_t a;
    assert_eq(0, gui_widget_create(fx.platform, row, GUI_WIDGET_CONTAINER, &a));

    /* 主轴占满父内容区，交叉轴固定。 */
    gui_geometry_t item;
    item.x = 0;
    item.y = 0;
    item.width = GUI_SIZE_FILL;
    item.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, a, &item));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &r));
    assert_eq(20, r.height);

    /* 主轴从 FILL 改成固定像素：交叉轴不受影响。 */
    item.width = GUI_SIZE_PX(50);
    assert_eq(0, gui_widget_set_geometry(fx.platform, a, &item));
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &r));
    assert_eq(50, r.width);
    assert_eq(20, r.height);

    gui_native_fixture_destroy(&fx);
}

/* ── 滚动与文本模式 ──────────────────────────────────────────────── */

TEST_FUNC(test_scroll_and_text_modes)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
    gui_widget_t container;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &container));

    gui_scroll_t s;
    s.horizontal = false;
    s.vertical = false;
    s.text_mode = GUI_TEXT_MODE_WRAP;
    s.text_align = GUI_TEXT_ALIGN_LEFT;

    /* 标签不滚动：默认组合可以接受。 */
    assert_eq(0, gui_widget_set_scroll(fx.platform, label, &s));

    s.horizontal = true;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, label, &s));
    assert_eq(EINVAL, errno);

    s.horizontal = false;
    s.vertical = true;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, label, &s));
    assert_eq(EINVAL, errno);

    /* 标签的文本模式可以改。 */
    s.vertical = false;
    s.text_mode = GUI_TEXT_MODE_CLIP;
    s.text_align = GUI_TEXT_ALIGN_CENTER;
    assert_eq(0, gui_widget_set_scroll(fx.platform, label, &s));

    /* 容器不承载长文本：文本字段必须是默认值。 */
    s.text_mode = GUI_TEXT_MODE_CLIP;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, container, &s));
    assert_eq(EINVAL, errno);

    s.text_mode = GUI_TEXT_MODE_WRAP;
    s.text_align = GUI_TEXT_ALIGN_RIGHT;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, container, &s));
    assert_eq(EINVAL, errno);

    /* 默认文本字段 + 滚动标志：容器可以接受。 */
    s.text_align = GUI_TEXT_ALIGN_LEFT;
    s.vertical = true;
    assert_eq(0, gui_widget_set_scroll(fx.platform, container, &s));

    gui_native_fixture_destroy(&fx);
}

/* ── 参数校验 ────────────────────────────────────────────────────── */

TEST_FUNC(test_layout_rejects_invalid_arguments)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(10);
    g.height = GUI_SIZE_PX(10);

    /* 空指针先被校验。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, root, NULL));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(NULL, root, &g));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_alignment(fx.platform, root, NULL));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_layout(fx.platform, root, NULL));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, root, NULL));
    assert_eq(EINVAL, errno);

    /* 无效身份。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, GUI_WIDGET_NONE, &g));
    assert_eq(EINVAL, errno);

    /* 越界的模式与锚点。 */
    gui_geometry_t bad_mode = g;
    bad_mode.width.mode = GUI_SIZE_MODE_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, root, &bad_mode));
    assert_eq(EINVAL, errno);

    gui_alignment_t bad_anchor = { GUI_ALIGN_COUNT, 0, 0 };
    errno = 0;
    assert_eq(-1, gui_widget_set_alignment(fx.platform, root, &bad_anchor));
    assert_eq(EINVAL, errno);

    gui_layout_t bad_flow;
    bad_flow.flow = GUI_FLOW_COUNT;
    bad_flow.main_align = GUI_FLOW_ALIGN_START;
    bad_flow.cross_align = GUI_FLOW_ALIGN_START;
    bad_flow.grow = 0;
    bad_flow.gap = 0;
    bad_flow.pad_top = 0;
    bad_flow.pad_bottom = 0;
    bad_flow.pad_left = 0;
    bad_flow.pad_right = 0;
    errno = 0;
    assert_eq(-1, gui_widget_set_layout(fx.platform, root, &bad_flow));
    assert_eq(EINVAL, errno);

    /* 标签是叶子：排布对它没有意义。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_layout(fx.platform, label, &bad_flow));
    assert_eq(EOPNOTSUPP, errno);

    gui_scroll_t bad_text;
    bad_text.horizontal = false;
    bad_text.vertical = false;
    bad_text.text_mode = GUI_TEXT_MODE_COUNT;
    bad_text.text_align = GUI_TEXT_ALIGN_LEFT;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, root, &bad_text));
    assert_eq(EINVAL, errno);

    bad_text.text_mode = GUI_TEXT_MODE_WRAP;
    bad_text.text_align = GUI_TEXT_ALIGN_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, root, &bad_text));
    assert_eq(EINVAL, errno);

    gui_native_fixture_destroy(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_layout_public_types_compile),
    TEST_ENTRY(test_geometry_sets_absolute_position_and_size),
    TEST_ENTRY(test_alignment_anchors_with_offsets),
    TEST_ENTRY(test_geometry_and_alignment_keep_size_source),
    TEST_ENTRY(test_fill_tracks_parent_content_length),
    TEST_ENTRY(test_content_parent_with_fill_child_is_rejected),
    TEST_ENTRY(test_flow_row_and_column_place_children),
    TEST_ENTRY(test_flow_main_alignment_centers_group),
    TEST_ENTRY(test_flow_item_size_source_is_kept),
    TEST_ENTRY(test_scroll_and_text_modes),
    TEST_ENTRY(test_layout_rejects_invalid_arguments),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
