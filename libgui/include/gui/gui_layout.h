#ifndef _GUI_LAYOUT_H
#define _GUI_LAYOUT_H

/*
 * gui_layout.h — 类型化的几何 / 布局 / 文本模式（GUI-2b）
 *
 * 公共契约：
 *   - 尺寸、位置、布局与文本模式全部是**值类型**：调用方描述"想要什么"，
 *     库负责把它落到原生控件上。
 *   - 三个命名空间互不冲突：`GUI_SIZE_MODE_*` 是尺寸**模式**枚举，
 *     `GUI_SIZE_*` 是构造出 `gui_size_t` 的**表达式**，`gui_size_t.mode`
 *     才是真正生效的值。
 *   - 本头不出现任何后端符号：它是后端无关的描述层。
 *
 * 尺寸模式（每一轴各一个）：
 *   - PX      固定像素；
 *   - CONTENT 由内容决定（文字或子项）；
 *   - FILL    占满父控件的**内容区**（父的内边距已经扣掉），不扣兄弟占用。
 *
 * 尺寸循环契约：父控件在某一轴上是 CONTENT，而它的子控件在同一轴上是
 * FILL 时，两者是互相依赖的尺寸循环。这样的组合一律被拒绝（EINVAL），
 * 而且**在改变任何状态之前**就拒绝：失败不留部分修改。
 */

#include <stdbool.h>
#include <stdint.h>

#include <gui/gui_widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 尺寸 ──────────────────────────────────────────────────────────── */

/**
 * @brief 尺寸模式
 *
 * `GUI_SIZE_MODE_COUNT` 是非法哨兵（模式数量），不得省略：模式越界的参数
 * 必须能被公共 API 拒绝。
 */
typedef enum {
    GUI_SIZE_MODE_PX = 0, /* 固定像素 */
    GUI_SIZE_MODE_CONTENT, /* 由内容决定 */
    GUI_SIZE_MODE_FILL,    /* 占满父控件内容区 */
    GUI_SIZE_MODE_COUNT    /* 非法哨兵 */
} gui_size_mode_t;

/**
 * @brief 一个轴上的尺寸
 *
 * `mode` 决定 `value` 是否被使用：PX 用它，另外两种模式忽略它。
 */
typedef struct {
    gui_size_mode_t mode;
    int32_t value;
} gui_size_t;

/*
 * 尺寸构造是**零值可用的表达式**：C 与 C++ 的字面量语法不同（复合字面量
 * vs 聚合初始化），两边都要能编译。
 */
#ifdef __cplusplus
#define GUI_SIZE_PX(v) (gui_size_t{ GUI_SIZE_MODE_PX, (int32_t)(v) })
#define GUI_SIZE_CONTENT (gui_size_t{ GUI_SIZE_MODE_CONTENT, 0 })
#define GUI_SIZE_FILL (gui_size_t{ GUI_SIZE_MODE_FILL, 0 })
#else
#define GUI_SIZE_PX(v) ((gui_size_t){ GUI_SIZE_MODE_PX, (int32_t)(v) })
#define GUI_SIZE_CONTENT ((gui_size_t){ GUI_SIZE_MODE_CONTENT, 0 })
#define GUI_SIZE_FILL ((gui_size_t){ GUI_SIZE_MODE_FILL, 0 })
#endif

/**
 * @brief 控件的几何（位置 + 尺寸）
 *
 * 位置是**绝对**的（相对父控件内容区左上角），与锚点对齐是两套互不覆盖
 * 的定位方式：设置其中一种不会抹掉另一种的尺寸来源。
 */
typedef struct {
    int32_t x;
    int32_t y;
    gui_size_t width;
    gui_size_t height;
} gui_geometry_t;

/* ── 对齐 ──────────────────────────────────────────────────────────── */

/**
 * @brief 锚点
 *
 * `GUI_ALIGN_COUNT` 是非法哨兵（锚点数量），不得省略。
 */
typedef enum {
    GUI_ALIGN_TOP_LEFT = 0,
    GUI_ALIGN_TOP_MID,
    GUI_ALIGN_TOP_RIGHT,
    GUI_ALIGN_MID_LEFT,
    GUI_ALIGN_MID,
    GUI_ALIGN_MID_RIGHT,
    GUI_ALIGN_BOTTOM_LEFT,
    GUI_ALIGN_BOTTOM_MID,
    GUI_ALIGN_BOTTOM_RIGHT,
    GUI_ALIGN_COUNT /* 非法哨兵 */
} gui_align_t;

/**
 * @brief 锚点 + 偏移的对齐方式
 *
 * 偏移相对锚点计算，正方向与几何坐标一致。
 */
typedef struct {
    gui_align_t anchor;
    int32_t x_offset;
    int32_t y_offset;
} gui_alignment_t;

/* ── 布局 ──────────────────────────────────────────────────────────── */

/**
 * @brief 子项的排布方向
 */
typedef enum {
    GUI_FLOW_NONE = 0, /* 不排布：子项各自定位 */
    GUI_FLOW_ROW,      /* 横向排布 */
    GUI_FLOW_COLUMN,   /* 纵向排布 */
    GUI_FLOW_COUNT     /* 非法哨兵 */
} gui_flow_t;

/**
 * @brief 排布里的对齐方式
 */
typedef enum {
    GUI_FLOW_ALIGN_START = 0, /* 靠主轴起点 */
    GUI_FLOW_ALIGN_CENTER,    /* 居中 */
    GUI_FLOW_ALIGN_END,       /* 靠主轴终点 */
    GUI_FLOW_ALIGN_COUNT      /* 非法哨兵 */
} gui_flow_align_t;

/**
 * @brief 容器的布局
 *
 * `grow` 是子项在主轴上的伸展权重；`gap` 是主轴上相邻子项之间的间距；
 * 四个 `pad_*` 是容器自己的内边距。
 */
typedef struct {
    gui_flow_t flow;
    gui_flow_align_t main_align;
    gui_flow_align_t cross_align;
    uint32_t grow;
    int32_t gap;
    int32_t pad_top;
    int32_t pad_bottom;
    int32_t pad_left;
    int32_t pad_right;
} gui_layout_t;

/* ── 文本与滚动 ────────────────────────────────────────────────────── */

/**
 * @brief 长文本的处理方式
 */
typedef enum {
    GUI_TEXT_MODE_WRAP = 0, /* 折行 */
    GUI_TEXT_MODE_CLIP,     /* 裁掉多余部分 */
    GUI_TEXT_MODE_COUNT     /* 非法哨兵 */
} gui_text_mode_t;

/**
 * @brief 文本的对齐方式
 */
typedef enum {
    GUI_TEXT_ALIGN_LEFT = 0,
    GUI_TEXT_ALIGN_CENTER,
    GUI_TEXT_ALIGN_RIGHT,
    GUI_TEXT_ALIGN_COUNT /* 非法哨兵 */
} gui_text_align_t;

/**
 * @brief 滚动与文本模式
 *
 * 标签不滚动：它的 `horizontal` / `vertical` 必须是 false。容器与按钮
 * 不承载长文本：它们的 `text_mode` / `text_align` 必须是默认值。
 */
typedef struct {
    bool horizontal;
    bool vertical;
    gui_text_mode_t text_mode;
    gui_text_align_t text_align;
} gui_scroll_t;

/* ── 设置器 ────────────────────────────────────────────────────────── */

/**
 * @brief 设置控件的几何（绝对位置 + 两轴尺寸）
 *
 * 位置写成绝对坐标（相对父控件内容区左上角），尺寸按各自的模式落地。
 * 与锚点对齐是两套定位方式：本函数把控件切成绝对定位，但不改变它的
 * 尺寸来源。
 *
 * 失败时不发生部分修改：尺寸循环、非法模式与非法参数都在改变任何状态
 * 之前被拒绝。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param geometry 几何描述，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法或尺寸循环，
 *         EOPNOTSUPP 该控件种类不接受几何）
 */
int gui_widget_set_geometry(gui_platform_t *platform, gui_widget_t widget,
                            const gui_geometry_t *geometry);

/**
 * @brief 设置控件的锚点对齐
 *
 * 对齐把控件切换成锚点定位（偏移相对锚点计算），但不改变它的尺寸来源。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param alignment 对齐描述，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法）
 */
int gui_widget_set_alignment(gui_platform_t *platform, gui_widget_t widget,
                             const gui_alignment_t *alignment);

/**
 * @brief 设置容器的布局
 *
 * 只有容器与按钮可以拥有排布：标签是叶子，布局对它没有意义。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param layout   布局描述，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法，EOPNOTSUPP 不是容器）
 */
int gui_widget_set_layout(gui_platform_t *platform, gui_widget_t widget, const gui_layout_t *layout);

/**
 * @brief 设置滚动与文本模式
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param scroll   滚动描述，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法或该种类不接受这个
 *         滚动/文本组合，EOPNOTSUPP 该控件种类不接受滚动）
 */
int gui_widget_set_scroll(gui_platform_t *platform, gui_widget_t widget, const gui_scroll_t *scroll);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_LAYOUT_H */
