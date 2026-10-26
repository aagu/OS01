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

#include <gui/gui_theme.h>
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

/* ── 外观 ──────────────────────────────────────────────────────────── */

/**
 * @brief 表面形态
 *
 * 形态是"这个表面是什么"的语义分类，它决定**哪些控件种类**可以使用它：
 *
 *   - ROOT    只给平台的根容器（全屏底板）；
 *   - PANEL   普通面板；
 *   - CARD    卡片；
 *   - ACTION  动作控件（按钮）；
 *   - PLAIN   素面（标签、透明顶栏、返回按钮）。
 *
 * `GUI_VARIANT_COUNT` 是非法哨兵（形态数量），不得省略。
 */
typedef enum {
    GUI_VARIANT_ROOT = 0,
    GUI_VARIANT_PANEL,
    GUI_VARIANT_CARD,
    GUI_VARIANT_ACTION,
    GUI_VARIANT_PLAIN,
    GUI_VARIANT_COUNT /* 非法哨兵 */
} gui_variant_t;

/**
 * @brief 色彩角色
 *
 * 角色与设计令牌表的字段一一对应：每个角色都指向令牌表里的一个颜色，
 * 界面代码因此不必出现裸颜色字面量。
 *
 * `GUI_COLOR_COUNT` 是非法哨兵（角色数量），不得省略。
 */
typedef enum {
    GUI_COLOR_PRIMARY = 0,
    GUI_COLOR_ON_PRIMARY,
    GUI_COLOR_PRIMARY_CONTAINER,
    GUI_COLOR_ON_PRIMARY_CONTAINER,
    GUI_COLOR_SECONDARY,
    GUI_COLOR_ON_SECONDARY,
    GUI_COLOR_SECONDARY_CONTAINER,
    GUI_COLOR_ON_SECONDARY_CONTAINER,
    GUI_COLOR_TERTIARY,
    GUI_COLOR_ON_TERTIARY,
    GUI_COLOR_TERTIARY_CONTAINER,
    GUI_COLOR_ON_TERTIARY_CONTAINER,
    GUI_COLOR_ERROR,
    GUI_COLOR_ON_ERROR,
    GUI_COLOR_ERROR_CONTAINER,
    GUI_COLOR_ON_ERROR_CONTAINER,
    GUI_COLOR_SUCCESS,
    GUI_COLOR_ON_SUCCESS,
    GUI_COLOR_SUCCESS_CONTAINER,
    GUI_COLOR_ON_SUCCESS_CONTAINER,
    GUI_COLOR_SURFACE,
    GUI_COLOR_SURFACE_DIM,
    GUI_COLOR_ON_SURFACE,
    GUI_COLOR_SURFACE_VARIANT,
    GUI_COLOR_ON_SURFACE_VARIANT,
    GUI_COLOR_SURFACE_CONTAINER_LOW,
    GUI_COLOR_SURFACE_CONTAINER,
    GUI_COLOR_SURFACE_CONTAINER_HIGH,
    GUI_COLOR_SURFACE_CONTAINER_HIGHEST,
    GUI_COLOR_OUTLINE,
    GUI_COLOR_OUTLINE_VARIANT,
    GUI_COLOR_INVERSE_SURFACE,
    GUI_COLOR_INVERSE_ON_SURFACE,
    GUI_COLOR_SCRIM,
    GUI_COLOR_SHADOW,
    GUI_COLOR_COUNT /* 非法哨兵 */
} gui_color_role_t;

/**
 * @brief 控件的外观
 *
 * 三个色彩角色分别是底色、文字色与普通边框色；字号、圆角与 elevation
 * 都是令牌表的层级；三个布尔量分别表示"底色是否填充"、"是否圆角"与
 * "是否画普通边框"。
 *
 * 普通边框与主题的**焦点环**是两回事：焦点环由主题的 FOCUSED 状态负责，
 * 外观永远不关闭它。
 */
typedef struct {
    gui_variant_t variant;
    gui_color_role_t bg_role;
    gui_color_role_t text_role;
    gui_color_role_t border_role;
    gui_theme_font_t font;
    gui_theme_radius_t radius;
    gui_theme_elevation_t elevation;
    bool filled;
    bool rounded;
    bool bordered;
} gui_appearance_t;

/*
 * 默认外观：面板形态 + 卡片表面 / 主要文字 / 细分割线 + 正文 / 中圆角 /
 * 无 elevation，填充、圆角，不画普通边框。
 */
#ifdef __cplusplus
#define GUI_APPEARANCE_DEFAULT                                                     \
    (gui_appearance_t{ GUI_VARIANT_PANEL, GUI_COLOR_SURFACE_CONTAINER,              \
                       GUI_COLOR_ON_SURFACE, GUI_COLOR_OUTLINE_VARIANT,            \
                       GUI_THEME_FONT_BODY, GUI_THEME_RADIUS_MD,                    \
                       GUI_THEME_ELEVATION_0, true, true, false })
#else
#define GUI_APPEARANCE_DEFAULT                                                     \
    ((gui_appearance_t){ GUI_VARIANT_PANEL, GUI_COLOR_SURFACE_CONTAINER,           \
                         GUI_COLOR_ON_SURFACE, GUI_COLOR_OUTLINE_VARIANT,          \
                         GUI_THEME_FONT_BODY, GUI_THEME_RADIUS_MD,                  \
                         GUI_THEME_ELEVATION_0, true, true, false })
#endif

/* ── 设置器 ────────────────────────────────────────────────────────── */

/**
 * @brief 设置控件的外观
 *
 * 形态与控件种类必须相容，否则被拒绝：标签只能用 PLAIN，容器只能用
 * PANEL / CARD / PLAIN（根容器还可以用 ROOT），按钮只能用 ACTION / CARD /
 * PLAIN。ROOT 形态还必须是透明、无边框、无圆角、无 elevation 的底板。
 *
 * 外观写的样式：普通状态（填充、文字、字体、圆角、边框、阴影、内边距）
 * 全部由这里决定；主题的 hover / pressed / disabled 状态样式原样保留。
 * 聚焦状态只额外写**填充色与文字色**：填充**就是 primary** 的控件聚焦时
 * 换成主题的 primary_container / on_primary_container（否则 primary 的焦点
 * 环会被同色填充吞掉）；其余填充色聚焦时保留自己的颜色。主题的焦点环
 * （primary 2px 环）不被外观层触碰，因此不会被普通局部边框遮掉。
 *
 * 失败时不发生部分修改：所有校验都在改动任何状态之前完成。
 *
 * @param platform  平台上下文句柄
 * @param widget    控件句柄
 * @param appearance 外观描述，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份/角色非法或形态与控件
 *         种类不相容）
 */
int gui_widget_set_appearance(gui_platform_t *platform, gui_widget_t widget,
                              const gui_appearance_t *appearance);

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
 * 像素尺寸必须能被后端表达：负值与超出后端坐标范围的值都以 EINVAL
 * 拒绝（不先截断成一个不同的值再接受）。
 *
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法、尺寸不可表达或
 *         构成尺寸循环）
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
 * 标签是叶子：它不接受排布，但可以在流式父里用**伸展权重**去适配宽度
 * （那一支要求 flow 为 NONE、gap 与四向 padding 均为 0，只应用伸展
 * 权重）；其余情况以 EOPNOTSUPP 拒绝。
 *
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法，
 *         EOPNOTSUPP 该控件种类不接受这组布局字段）
 */
int gui_widget_set_layout(gui_platform_t *platform, gui_widget_t widget, const gui_layout_t *layout);

/**
 * @brief 设置滚动与文本模式
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param scroll   滚动描述，不可为 NULL
 * 标签不滚动：它的滚动标志必须都是 false。容器与按钮不承载长文本：
 * 它们的文本字段必须是默认值。这两类**类型不匹配**以 EOPNOTSUPP 拒绝；
 * 角色/枚举越界等参数问题以 EINVAL 拒绝。
 *
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份/枚举非法，
 *         EOPNOTSUPP 该控件种类不接受这组滚动/文本字段）
 */
int gui_widget_set_scroll(gui_platform_t *platform, gui_widget_t widget, const gui_scroll_t *scroll);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_LAYOUT_H */
