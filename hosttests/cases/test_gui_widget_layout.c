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
#include <gui/gui_theme.h>
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

/*
 * 区域里"有墨"的行数，以及整块区域里第一个有墨的列。
 *
 * 墨 = 与背景不同的像素。标签是透明的，它身后是屏幕底色（surface），
 * 因此背景是确定的。
 */
static uint32_t count_ink_rows(gui_platform_t *platform, int32_t x, int32_t y, int32_t w,
                               int32_t h, int32_t *out_first_ink_x)
{
    uint32_t background = gui_theme_colors()->surface;
    uint32_t rows = 0;
    int32_t first_x = -1;

    for (int32_t row = 0; row < h; row++) {
        bool ink = false;
        for (int32_t col = 0; col < w; col++) {
            uint32_t pixel =
                gui_platform_get_pixel(platform, (uint32_t)(x + col), (uint32_t)(y + row));
            if (pixel != background) {
                ink = true;
                if (first_x < 0) {
                    first_x = col;
                }
                break;
            }
        }
        if (ink) {
            rows++;
        }
    }
    if (out_first_ink_x) {
        *out_first_ink_x = first_x;
    }
    return rows;
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

TEST_FUNC(test_size_constructors_compile)
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

TEST_FUNC(test_fill_per_axis_and_update_order)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* 根留一点内边距：这样"父内容区"与"父总尺寸"才是两件事。 */
    gui_layout_t root_layout;
    root_layout.flow = GUI_FLOW_NONE;
    root_layout.main_align = GUI_FLOW_ALIGN_START;
    root_layout.cross_align = GUI_FLOW_ALIGN_START;
    root_layout.grow = 0;
    root_layout.gap = 0;
    root_layout.pad_top = 20;
    root_layout.pad_bottom = 20;
    root_layout.pad_left = 20;
    root_layout.pad_right = 20;
    assert_eq(0, gui_widget_set_layout(fx.platform, root, &root_layout));

    gui_layout_t row_layout;
    row_layout.flow = GUI_FLOW_ROW;
    row_layout.main_align = GUI_FLOW_ALIGN_START;
    row_layout.cross_align = GUI_FLOW_ALIGN_START;
    row_layout.grow = 0;
    row_layout.gap = 0;
    row_layout.pad_top = 0;
    row_layout.pad_bottom = 0;
    row_layout.pad_left = 0;
    row_layout.pad_right = 0;

    /* ── 1. 交换设置顺序等价 ───────────────────────────────────────
     *
     * "先设几何后设布局"与"先设布局后设几何"必须落到同一个结果：尺寸
     * 来源与排布是**两套**独立的描述，谁先谁后都不改变结论。 */
    gui_widget_t r1;
    gui_widget_t r2;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &r1));
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &r2));

    gui_geometry_t box;
    box.x = 0;
    box.y = 0;
    box.width = GUI_SIZE_PX(200);
    box.height = GUI_SIZE_PX(100);

    /* r1：先几何后布局。r2：先布局后几何。 */
    assert_eq(0, gui_widget_set_geometry(fx.platform, r1, &box));
    assert_eq(0, gui_widget_set_layout(fx.platform, r1, &row_layout));
    assert_eq(0, gui_widget_set_layout(fx.platform, r2, &row_layout));
    assert_eq(0, gui_widget_set_geometry(fx.platform, r2, &box));

    /* 父内容区的全长（在子项存在之前量：探针才是第一个排布项）。 */
    int32_t cw = 0;
    int32_t ch = 0;
    measure_content_box(fx.platform, r1, NULL, NULL, &cw, &ch);

    gui_widget_t d1;
    gui_widget_t d2;
    assert_eq(0, gui_widget_create(fx.platform, r1, GUI_WIDGET_CONTAINER, &d1));
    assert_eq(0, gui_widget_create(fx.platform, r2, GUI_WIDGET_CONTAINER, &d2));

    gui_geometry_t fill_w;
    fill_w.x = 0;
    fill_w.y = 0;
    fill_w.width = GUI_SIZE_FILL;
    fill_w.height = GUI_SIZE_PX(30);
    assert_eq(0, gui_widget_set_geometry(fx.platform, d1, &fill_w));
    assert_eq(0, gui_widget_set_geometry(fx.platform, d2, &fill_w));

    gui_rect_t b1;
    gui_rect_t b2;
    assert_eq(0, gui_widget_get_bounds(fx.platform, d1, &b1));
    assert_eq(0, gui_widget_get_bounds(fx.platform, d2, &b2));
    /* 交换设置顺序不改变结论。 */
    assert_eq(b1.width, b2.width);
    assert_eq(b1.height, b2.height);
    assert_eq(b1.x, b2.x);
    assert_eq(b1.y, b2.y);

    /* ── 2. ROW / COLUMN 的主轴与交叉轴 ────────────────────────────
     *
     * ROW 的主轴是横向、交叉轴是纵向；COLUMN 反过来。FILL 逐轴生效。 */

    /* 主轴（横向）FILL：宽度就是父内容区的全长。 */
    assert_eq(cw, b1.width);
    assert_eq(30, b1.height);

    /* 交叉轴（纵向）FILL：高度就是父内容区的全长，宽度不受影响。 */
    gui_geometry_t fill_h;
    fill_h.x = 0;
    fill_h.y = 0;
    fill_h.width = GUI_SIZE_PX(40);
    fill_h.height = GUI_SIZE_FILL;
    assert_eq(0, gui_widget_set_geometry(fx.platform, d1, &fill_h));
    gui_rect_t b3;
    assert_eq(0, gui_widget_get_bounds(fx.platform, d1, &b3));
    assert_eq(40, b3.width);
    assert_eq(ch, b3.height);

    /* COLUMN：主轴变成纵向。 */
    gui_widget_t column;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &column));
    assert_eq(0, gui_widget_set_geometry(fx.platform, column, &box));
    gui_layout_t column_layout = row_layout;
    column_layout.flow = GUI_FLOW_COLUMN;
    assert_eq(0, gui_widget_set_layout(fx.platform, column, &column_layout));

    gui_widget_t c1;
    assert_eq(0, gui_widget_create(fx.platform, column, GUI_WIDGET_CONTAINER, &c1));
    gui_geometry_t fill_v;
    fill_v.x = 0;
    fill_v.y = 0;
    fill_v.width = GUI_SIZE_PX(30);
    fill_v.height = GUI_SIZE_FILL;
    assert_eq(0, gui_widget_set_geometry(fx.platform, c1, &fill_v));
    gui_rect_t b4;
    assert_eq(0, gui_widget_get_bounds(fx.platform, c1, &b4));
    assert_eq(30, b4.width);
    assert_eq(ch, b4.height);

    /* ── 3. FILL → PX50：另一轴不变 ──────────────────────────────── */
    gui_geometry_t fixed;
    fixed.x = 0;
    fixed.y = 0;
    fixed.width = GUI_SIZE_PX(50);
    fixed.height = GUI_SIZE_FILL;
    assert_eq(0, gui_widget_set_geometry(fx.platform, d1, &fixed));
    gui_rect_t b5;
    assert_eq(0, gui_widget_get_bounds(fx.platform, d1, &b5));
    assert_eq(50, b5.width);
    assert_eq(b3.height, b5.height);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_content_fill_cycle_rejected_atomically)
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

TEST_FUNC(test_wrap_clip_and_text_alignment)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    const char *long_text = "The quick brown fox jumps over the lazy dog again and again";
    assert_eq(0, gui_widget_set_text(fx.platform, label, long_text));

    /* 固定宽度：折行与裁切的分歧就发生在这个宽度上。高度由内容决定。 */
    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(80);
    g.height = GUI_SIZE_CONTENT;
    assert_eq(0, gui_widget_set_geometry(fx.platform, label, &g));

    gui_scroll_t scroll;
    scroll.horizontal = false;
    scroll.vertical = false;
    scroll.text_mode = GUI_TEXT_MODE_WRAP;
    scroll.text_align = GUI_TEXT_ALIGN_LEFT;

    /* ── 折行：长文本被排成多行 ──────────────────────────────────── */
    assert_eq(0, gui_widget_set_scroll(fx.platform, label, &scroll));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t wrap_rect;
    assert_eq(0, gui_widget_get_bounds(fx.platform, label, &wrap_rect));
    uint32_t wrap_rows = count_ink_rows(fx.platform, wrap_rect.x, wrap_rect.y, wrap_rect.width,
                                        wrap_rect.height, NULL);
    assert_true(wrap_rows >= 2);

    /* ── 裁切：长文本被切掉，只留一行 ────────────────────────────── */
    scroll.text_mode = GUI_TEXT_MODE_CLIP;
    assert_eq(0, gui_widget_set_scroll(fx.platform, label, &scroll));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t clip_rect;
    assert_eq(0, gui_widget_get_bounds(fx.platform, label, &clip_rect));
    uint32_t clip_rows = count_ink_rows(fx.platform, clip_rect.x, clip_rect.y, clip_rect.width,
                                        clip_rect.height, NULL);

    /* 折行与裁切必须**看得见**地不同。 */
    assert_true(clip_rows < wrap_rows);
    assert_true(clip_rect.height < wrap_rect.height);

    /* ── 文本对齐：起始像素随对齐方式移动 ────────────────────────── */
    gui_widget_t short_label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &short_label));
    assert_eq(0, gui_widget_set_text(fx.platform, short_label, "Hi"));
    gui_geometry_t wide;
    wide.x = 0;
    wide.y = 220; /* 避开上面那个折行标签，两者不许重叠 */
    wide.width = GUI_SIZE_PX(200);
    wide.height = GUI_SIZE_CONTENT;
    assert_eq(0, gui_widget_set_geometry(fx.platform, short_label, &wide));

    scroll.text_mode = GUI_TEXT_MODE_WRAP;
    scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    assert_eq(0, gui_widget_set_scroll(fx.platform, short_label, &scroll));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t left_rect;
    assert_eq(0, gui_widget_get_bounds(fx.platform, short_label, &left_rect));
    int32_t left_x = -1;
    (void)count_ink_rows(fx.platform, left_rect.x, left_rect.y, left_rect.width, left_rect.height,
                          &left_x);
    assert_true(left_x >= 0);

    scroll.text_align = GUI_TEXT_ALIGN_CENTER;
    assert_eq(0, gui_widget_set_scroll(fx.platform, short_label, &scroll));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    gui_rect_t center_rect;
    assert_eq(0, gui_widget_get_bounds(fx.platform, short_label, &center_rect));
    int32_t center_x = -1;
    (void)count_ink_rows(fx.platform, center_rect.x, center_rect.y, center_rect.width,
                          center_rect.height, &center_x);
    assert_true(center_x > left_x);

    scroll.text_align = GUI_TEXT_ALIGN_RIGHT;
    assert_eq(0, gui_widget_set_scroll(fx.platform, short_label, &scroll));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    gui_rect_t right_rect;
    assert_eq(0, gui_widget_get_bounds(fx.platform, short_label, &right_rect));
    int32_t right_x = -1;
    (void)count_ink_rows(fx.platform, right_rect.x, right_rect.y, right_rect.width, right_rect.height,
                          &right_x);
    assert_true(right_x > center_x);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_flow_grow_padding_and_alignment)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    gui_geometry_t box;
    box.x = 0;
    box.y = 0;
    box.width = GUI_SIZE_PX(400);
    box.height = GUI_SIZE_PX(100);

    gui_layout_t row;
    row.flow = GUI_FLOW_ROW;
    row.main_align = GUI_FLOW_ALIGN_START;
    row.cross_align = GUI_FLOW_ALIGN_START;
    row.grow = 0;
    row.gap = 0;
    row.pad_top = 0;
    row.pad_bottom = 0;
    row.pad_left = 0;
    row.pad_right = 0;

    /* ── 伸展权重：剩余空间按权重分 ──────────────────────────────── */
    gui_widget_t parent;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &parent));
    assert_eq(0, gui_widget_set_geometry(fx.platform, parent, &box));
    assert_eq(0, gui_widget_set_layout(fx.platform, parent, &row));

    /* 父内容区的全长（在没有子项的时候量：探针才是第一个排布项）。 */
    int32_t content_w = 0;
    int32_t content_h = 0;
    measure_content_box(fx.platform, parent, NULL, NULL, &content_w, &content_h);
    assert_true(content_w > 0);

    /* 两个子项：伸展权重 1 与 2。 */
    gui_widget_t a;
    gui_widget_t b;
    assert_eq(0, gui_widget_create(fx.platform, parent, GUI_WIDGET_CONTAINER, &a));
    assert_eq(0, gui_widget_create(fx.platform, parent, GUI_WIDGET_CONTAINER, &b));

    gui_geometry_t item;
    item.x = 0;
    item.y = 0;
    item.width = GUI_SIZE_PX(40);
    item.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, a, &item));
    assert_eq(0, gui_widget_set_geometry(fx.platform, b, &item));

    gui_layout_t grow;
    grow.flow = GUI_FLOW_NONE;
    grow.main_align = GUI_FLOW_ALIGN_START;
    grow.cross_align = GUI_FLOW_ALIGN_START;
    grow.gap = 0;
    grow.pad_top = 0;
    grow.pad_bottom = 0;
    grow.pad_left = 0;
    grow.pad_right = 0;

    grow.grow = 1;
    assert_eq(0, gui_widget_set_layout(fx.platform, a, &grow));
    grow.grow = 2;
    assert_eq(0, gui_widget_set_layout(fx.platform, b, &grow));

    gui_rect_t ra;
    gui_rect_t rb;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &ra));
    assert_eq(0, gui_widget_get_bounds(fx.platform, b, &rb));

    /* 剩余空间按 1:2 分：两个伸展项把整条内容区分掉。 */
    assert_eq((uint32_t)content_w, (uint32_t)(ra.width + rb.width));
    /* a ≈ 1/3、b ≈ 2/3（整数除法的余数落在后一个项上）。 */
    assert_true(ra.width * 3 >= (uint32_t)content_w - 1);
    assert_true(ra.width * 3 <= (uint32_t)content_w + 1);
    assert_true(rb.width * 3 >= (uint32_t)content_w * 2 - 2);
    assert_true(rb.width * 3 <= (uint32_t)content_w * 2 + 2);

    /* 标签也能用伸展权重去适配流式父：那一支只应用 flex grow。 */
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
    assert_eq(0, gui_widget_set_text(fx.platform, label, "L"));
    gui_layout_t label_grow = grow;
    label_grow.grow = 3;
    assert_eq(0, gui_widget_set_layout(fx.platform, label, &label_grow));

    /* 标签不接受排布字段：非默认的容器字段仍然是 EOPNOTSUPP。 */
    gui_layout_t label_layout = label_grow;
    label_layout.gap = 4;
    errno = 0;
    assert_eq(-1, gui_widget_set_layout(fx.platform, label, &label_layout));
    assert_eq(EOPNOTSUPP, errno);

    /* 与 FILL 不同：FILL 拿到的是整条内容区，伸展权重拿到的是份额。 */
    gui_widget_t fill_parent;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &fill_parent));
    assert_eq(0, gui_widget_set_geometry(fx.platform, fill_parent, &box));
    assert_eq(0, gui_widget_set_layout(fx.platform, fill_parent, &row));
    gui_widget_t fill_child;
    assert_eq(0, gui_widget_create(fx.platform, fill_parent, GUI_WIDGET_CONTAINER, &fill_child));
    gui_geometry_t fill_geo;
    fill_geo.x = 0;
    fill_geo.y = 0;
    fill_geo.width = GUI_SIZE_FILL;
    fill_geo.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, fill_child, &fill_geo));
    gui_rect_t rf;
    assert_eq(0, gui_widget_get_bounds(fx.platform, fill_child, &rf));
    assert_eq((uint32_t)content_w, (uint32_t)rf.width);
    assert_true(rf.width != ra.width);

    /* ── 内边距：父的内容区变窄，伸展份额跟着变窄 ────────────────── */
    gui_layout_t padded = row;
    padded.pad_left = 20;
    padded.pad_right = 20;
    assert_eq(0, gui_widget_set_layout(fx.platform, parent, &padded));

    gui_rect_t pa;
    gui_rect_t pb;
    assert_eq(0, gui_widget_get_bounds(fx.platform, a, &pa));
    assert_eq(0, gui_widget_get_bounds(fx.platform, b, &pb));
    assert_eq((uint32_t)content_w - 40u, (uint32_t)(pa.width + pb.width));

    /* ── 对齐：整组在主轴上居中 ─────────────────────────────────── */
    gui_widget_t align_parent;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &align_parent));
    assert_eq(0, gui_widget_set_geometry(fx.platform, align_parent, &box));
    gui_layout_t centered = row;
    centered.main_align = GUI_FLOW_ALIGN_CENTER;
    assert_eq(0, gui_widget_set_layout(fx.platform, align_parent, &centered));

    int32_t origin_x = 0;
    int32_t align_content_w = 0;
    measure_content_box(fx.platform, align_parent, &origin_x, NULL, &align_content_w, NULL);

    gui_widget_t c1;
    gui_widget_t c2;
    assert_eq(0, gui_widget_create(fx.platform, align_parent, GUI_WIDGET_CONTAINER, &c1));
    assert_eq(0, gui_widget_create(fx.platform, align_parent, GUI_WIDGET_CONTAINER, &c2));
    gui_geometry_t fixed;
    fixed.x = 0;
    fixed.y = 0;
    fixed.width = GUI_SIZE_PX(40);
    fixed.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, c1, &fixed));
    assert_eq(0, gui_widget_set_geometry(fx.platform, c2, &fixed));

    gui_rect_t rc1;
    gui_rect_t rc2;
    assert_eq(0, gui_widget_get_bounds(fx.platform, c1, &rc1));
    assert_eq(0, gui_widget_get_bounds(fx.platform, c2, &rc2));
    /* 居中：整组（40 + 40）在内容区里居中。 */
    assert_eq(origin_x + (align_content_w - 80) / 2, rc1.x);
    assert_eq(rc1.x + 40, rc2.x);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_geometry_range_and_enum_validation)
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

    /* 标签是叶子：容器字段必须是默认值，否则 EOPNOTSUPP。 */
    gui_layout_t label_layout;
    label_layout.flow = GUI_FLOW_NONE;
    label_layout.main_align = GUI_FLOW_ALIGN_START;
    label_layout.cross_align = GUI_FLOW_ALIGN_START;
    label_layout.grow = 0;
    label_layout.gap = 4;
    label_layout.pad_top = 0;
    label_layout.pad_bottom = 0;
    label_layout.pad_left = 0;
    label_layout.pad_right = 0;
    errno = 0;
    assert_eq(-1, gui_widget_set_layout(fx.platform, label, &label_layout));
    assert_eq(EOPNOTSUPP, errno);

    /* 但伸展权重是允许的。 */
    label_layout.gap = 0;
    label_layout.grow = 2;
    assert_eq(0, gui_widget_set_layout(fx.platform, label, &label_layout));

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

    /* 类型不匹配是 EOPNOTSUPP（不是 EINVAL）：标签不滚动，容器不承载长文本。 */
    gui_scroll_t label_scroll;
    label_scroll.horizontal = true;
    label_scroll.vertical = false;
    label_scroll.text_mode = GUI_TEXT_MODE_WRAP;
    label_scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, label, &label_scroll));
    assert_eq(EOPNOTSUPP, errno);

    gui_scroll_t container_scroll;
    container_scroll.horizontal = false;
    container_scroll.vertical = false;
    container_scroll.text_mode = GUI_TEXT_MODE_CLIP;
    container_scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    errno = 0;
    assert_eq(-1, gui_widget_set_scroll(fx.platform, root, &container_scroll));
    assert_eq(EOPNOTSUPP, errno);

    /* ── 负尺寸与整数边界 ──────────────────────────────────────────
     *
     * 像素尺寸必须能被后端表达：负值与超出坐标范围的值都被拒绝，而且
     * **不先截断成一个不同的值再接受**。 */
    gui_widget_t box;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &box));
    gui_geometry_t ok_geo;
    ok_geo.x = 3;
    ok_geo.y = 4;
    ok_geo.width = GUI_SIZE_PX(30);
    ok_geo.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, box, &ok_geo));

    gui_rect_t before;
    assert_eq(0, gui_widget_get_bounds(fx.platform, box, &before));

    gui_geometry_t negative = ok_geo;
    negative.width = GUI_SIZE_PX(-1);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, box, &negative));
    assert_eq(EINVAL, errno);

    negative = ok_geo;
    negative.height = GUI_SIZE_PX(-100);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, box, &negative));
    assert_eq(EINVAL, errno);

    negative = ok_geo;
    negative.width = GUI_SIZE_PX(INT32_MIN);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, box, &negative));
    assert_eq(EINVAL, errno);

    /* 整数上界：超出后端坐标范围的值同样被拒绝。 */
    gui_geometry_t too_big = ok_geo;
    too_big.width = GUI_SIZE_PX(INT32_MAX);
    errno = 0;
    assert_eq(-1, gui_widget_set_geometry(fx.platform, box, &too_big));
    assert_eq(EINVAL, errno);

    /* 无部分修改：被拒绝的调用没有改动任何状态。 */
    gui_rect_t after;
    assert_eq(0, gui_widget_get_bounds(fx.platform, box, &after));
    assert_eq(before.x, after.x);
    assert_eq(before.y, after.y);
    assert_eq(before.width, after.width);
    assert_eq(before.height, after.height);

    /* ── get_bounds 失败清零 ─────────────────────────────────────── */
    gui_rect_t dirty;
    dirty.x = 111;
    dirty.y = 222;
    dirty.width = 333;
    dirty.height = 444;
    errno = 0;
    assert_eq(-1, gui_widget_get_bounds(fx.platform, GUI_WIDGET_NONE, &dirty));
    assert_eq(EINVAL, errno);
    assert_eq(0, dirty.x);
    assert_eq(0, dirty.y);
    assert_eq(0, dirty.width);
    assert_eq(0, dirty.height);

    gui_native_fixture_destroy(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_size_constructors_compile),
    TEST_ENTRY(test_geometry_sets_absolute_position_and_size),
    TEST_ENTRY(test_alignment_anchors_with_offsets),
    TEST_ENTRY(test_geometry_and_alignment_keep_size_source),
    TEST_ENTRY(test_fill_per_axis_and_update_order),
    TEST_ENTRY(test_content_fill_cycle_rejected_atomically),
    TEST_ENTRY(test_flow_row_and_column_place_children),
    TEST_ENTRY(test_flow_main_alignment_centers_group),
    TEST_ENTRY(test_flow_item_size_source_is_kept),
    TEST_ENTRY(test_wrap_clip_and_text_alignment),
    TEST_ENTRY(test_flow_grow_padding_and_alignment),
    TEST_ENTRY(test_geometry_range_and_enum_validation),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
