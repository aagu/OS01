#ifndef _GUI_WIDGET_H
#define _GUI_WIDGET_H

/*
 * gui_widget.h — 公共控件句柄（GUI-2b）
 *
 * 公共契约：
 *   - 句柄是**值类型**（只含一个 uint64_t 身份号），不是指针：调用方不能
 *     伪造身份，也不能把句柄强转成后端对象指针。
 *   - 零身份号是 `GUI_WIDGET_NONE`：它表示"没有控件"。
 *   - 身份号进程全局递增：平台销毁/重建都不重置、不复用、不回绕。
 *   - 本头只依赖平台头与标准整数/布尔类型，不出现任何后端符号。
 *
 * 句柄不延长对象寿命：它只是身份，不是所有权凭证。平台销毁之后所有句柄
 * 一律失效。
 */

#include <stdbool.h>
#include <stdint.h>

#include <gui/gui_platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 公共控件句柄（值类型）
 *
 * 唯一的成员是身份号：它是**不透明**的，调用方只能整体传递或比较，
 * 不能拆开使用，也不能当成指针。
 */
typedef struct { uint64_t id; } gui_widget_t;

/**
 * @brief 原生控件种类
 *
 * `GUI_WIDGET_KIND_COUNT` 是非法哨兵（种类数量），不得省略：种类越界的
 * 参数必须能被公共 API 拒绝。
 */
typedef enum {
    GUI_WIDGET_CONTAINER = 0, /* 容器：可以拥有子控件 */
    GUI_WIDGET_LABEL,         /* 标签：只显示文字 */
    GUI_WIDGET_BUTTON,        /* 按钮：可点击 */
    GUI_WIDGET_KIND_COUNT     /* 非法哨兵 */
} gui_widget_kind_t;

/**
 * @brief 控件在视口中的像素矩形
 *
 * 坐标原点在视口左上角；宽度/高度是像素数（不是右边界）。
 */
typedef struct { int32_t x, y, width, height; } gui_rect_t;

/*
 * NONE 是**零值构造表达式**，不是指针、不是整数常量。
 *
 * C 与 C++ 的字面量语法不同（复合字面量 vs 聚合初始化），两边都要能编译：
 * 公共头的 C11 consumer 与 C++17 consumer 都要过语法检查。
 */
#ifdef __cplusplus
#define GUI_WIDGET_NONE (gui_widget_t{ 0 })
#else
#define GUI_WIDGET_NONE ((gui_widget_t){ 0 })
#endif

/**
 * @brief 查询一个身份当前是否指向一个活着的控件
 *
 * 无效身份（零句柄、未知身份、已销毁身份、别的平台的身份）一律返回 false。
 * 本函数没有任何结构副作用：它不改变注册表、不创建也不释放任何东西。
 *
 * @param platform 平台上下文句柄
 * @param widget   待查询的控件句柄
 * @return true 表示该身份当前有效
 */
bool gui_widget_is_valid(gui_platform_t *platform, gui_widget_t widget);

/**
 * @brief 查询控件的种类
 *
 * 失败时 `*out` 被清成 `GUI_WIDGET_KIND_COUNT`（非法哨兵），并且**不发生
 * 部分修改**：失败时输出永远是被清零的非法值，不会残留上一次的种类。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param out      输出参数：控件种类，不可为 NULL
 * @return 0 成功；-1 失败（errno 已设置：无效身份为 EINVAL）
 */
int gui_widget_get_kind(gui_platform_t *platform, gui_widget_t widget, gui_widget_kind_t *out);

/**
 * @brief 创建一个原生控件
 *
 * 归属规则：父控件拥有子控件。
 *
 *   - 只有**容器**可以是根（`GUI_WIDGET_NONE` 父控件表示"本平台活动屏幕
 *     上的全屏根"）；其他种类不能作根。
 *   - 父控件必须是**同一个平台**上的活着的控件，且种类为 `CONTAINER` 或
 *     `BUTTON`：这两者都可以拥有子项（按钮的标题就是它的 LABEL 子项）。
 *   - `LABEL` 是叶子，不能作父。
 *
 * 失败时不发生部分修改：参数（平台、父控件、种类、输出指针）全部先被校验，
 * 再占用槽位、建原生对象、挂注册表观察者，最后才发布身份。
 *
 * @param platform 平台上下文句柄
 * @param parent   父控件（`GUI_WIDGET_NONE` 表示根容器）
 * @param kind     控件种类
 * @param out      输出参数：新控件的身份，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 参数/身份非法，ENOSPC 超出容量，
 *         ENOMEM 原生分配失败，EOVERFLOW 身份号耗尽）
 */
int gui_widget_create(gui_platform_t *platform, gui_widget_t parent, gui_widget_kind_t kind,
                      gui_widget_t *out);

/**
 * @brief 销毁一个控件（递归销毁它的子树）
 *
 * 销毁先让身份对公共查询失效，再真正删除原生对象。父控件被销毁时，它的
 * 整棵子树一起消失，所有旧身份立即失效。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效，EBUSY 正在业务回调中）
 */
int gui_widget_destroy(gui_platform_t *platform, gui_widget_t widget);

/**
 * @brief 清空一个容器（删掉它的子项，容器本身留下）
 *
 * @param platform 平台上下文句柄
 * @param container 容器句柄
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效，EOPNOTSUPP 不是容器，
 *         EBUSY 正在业务回调中）
 */
int gui_widget_clear(gui_platform_t *platform, gui_widget_t container);

/**
 * @brief 设置标签文字
 *
 * 文字被**复制**进库：调用方的缓冲可以在返回后立刻改掉或释放。`text` 为
 * NULL 时按空串处理。
 *
 * @param platform 平台上下文句柄
 * @param label    标签句柄
 * @param text     文字（可为 NULL）
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效，EOPNOTSUPP 不是标签，
 *         ENOMEM 分配失败）
 */
int gui_widget_set_text(gui_platform_t *platform, gui_widget_t label, const char *text);

/**
 * @brief 设置控件可见性
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param visible  true 可见
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效）
 */
int gui_widget_set_visible(gui_platform_t *platform, gui_widget_t widget, bool visible);

/**
 * @brief 设置控件使能状态
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param enabled  true 可用
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效）
 */
int gui_widget_set_enabled(gui_platform_t *platform, gui_widget_t widget, bool enabled);

/**
 * @brief 查询控件在视口中的像素矩形
 *
 * 查询会先让当前布局完成，因此返回的是**布局之后**的矩形。失败时 `*out`
 * 被清零，不发生部分修改。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param out      输出参数：像素矩形，不可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效或输出指针为空）
 */
int gui_widget_get_bounds(gui_platform_t *platform, gui_widget_t widget, gui_rect_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_WIDGET_H */
