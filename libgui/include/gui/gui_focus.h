#ifndef _GUI_FOCUS_H
#define _GUI_FOCUS_H

/*
 * gui_focus.h — 公共焦点 API（GUI-2b Task 7）
 *
 * 焦点引擎只有一个：原生登记与诊断(raw)登记共用同一份登记表、同一套选择
 * 提交路径。本头只暴露"业务能做什么"，不暴露后端类型，也不暴露任何
 * adopt / unwrap / get-native 之类的后门。
 *
 * 错误契约：int API 返回 0 成功、-1 失败；失败时 errno 被设置，输出身份
 * 被清零，**不做任何部分修改**。
 */

#include <gui/gui_input.h>
#include <gui/gui_widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 焦点登记策略
 *
 * TAB     只参与键盘 Tab 导航；点击它**不**夺走键盘焦点。
 * CLICK   参与键盘 Tab 导航，点击它会夺走键盘焦点。
 * STRONG  参与键盘 Tab 导航，点击它会夺走键盘焦点。
 */
typedef enum {
    GUI_FOCUS_TAB = 0,
    GUI_FOCUS_CLICK,
    GUI_FOCUS_STRONG
} gui_focus_policy_t;

/**
 * @brief 首次焦点建立策略
 *
 * AUTO_FIRST  第一个可用的 Tab 登记建立选择。
 * MANUAL      不自动建立选择：保持 NONE，直到第一次 Tab 或一次显式设置。
 */
typedef enum {
    GUI_FOCUS_INITIAL_AUTO_FIRST = 0,
    GUI_FOCUS_INITIAL_MANUAL
} gui_focus_initial_t;

/**
 * @brief 焦点移动方向
 */
typedef enum {
    GUI_FOCUS_FORWARD = 0,
    GUI_FOCUS_BACKWARD
} gui_focus_direction_t;

/**
 * @brief 登记一个原生控件参与焦点管理
 *
 * 登记仅限 BUTTON。重复登记更新策略并**保留原位置**（不占新容量）。
 * 登记即锁定登记模式：input 的第一次成功登记决定它是原生模式还是诊断
 * 模式，此后混用两种登记会被拒绝。
 *
 * @param input  输入对象句柄
 * @param widget 控件身份（必须属于 input 自己的平台）
 * @param policy 焦点策略
 * @return 0 成功；-1 失败（errno：EINVAL 无效参数/枚举/失效或外平台身份，
 *         EOPNOTSUPP 控件种类不支持，EBUSY 登记模式冲突或业务回调内调用，
 *         ENOSPC 登记表已满，ENOMEM 观察者安装失败）
 */
int gui_focus_register(gui_input_t *input, gui_widget_t widget, gui_focus_policy_t policy);

/**
 * @brief 注销一个原生焦点登记
 *
 * 注销会摘掉该条目的删除观察者；被注销的原生控件不再响应指针夺焦。
 * 注销当前焦点项时，焦点按"移除前的位置"落到后继上。
 *
 * @return 0 成功；-1 失败（errno：EINVAL 未登记或无效身份，
 *         EOPNOTSUPP 诊断登记模式，EBUSY 业务回调内调用）
 */
int gui_focus_unregister(gui_input_t *input, gui_widget_t widget);

/**
 * @brief 设置当前焦点
 *
 * `widget` 为 GUI_WIDGET_NONE 时表示**显式清空**：焦点保持 NONE，直到下一次
 * 成功的选择为止；后续登记或刷新都不会把它复活。
 *
 * @return 0 成功；-1 失败（errno：EINVAL 未登记/无效身份/UNSET 模式下设置了
 *         非 NONE 目标，EACCES 目标当前不可用，EOPNOTSUPP 诊断登记模式，
 *         EBUSY 业务回调内调用）
 */
int gui_focus_set(gui_input_t *input, gui_widget_t widget);

/**
 * @brief 查询当前焦点
 *
 * 只读查询：在业务回调里**允许**调用，返回最后一次已提交的选择。
 *
 * @param out 输出参数（不可为 NULL）；失败时被清零为 GUI_WIDGET_NONE
 * @return 0 成功；-1 失败（errno：EINVAL 输出参数非法，
 *         EOPNOTSUPP 诊断登记模式）
 */
int gui_focus_get(gui_input_t *input, gui_widget_t *out);

/**
 * @brief 按方向移动焦点
 *
 * @param direction 移动方向
 * @return 0 成功（没有候选时成功且焦点为空）；-1 失败（errno：EINVAL
 *         无效枚举，EOPNOTSUPP 诊断登记模式，EBUSY 业务回调内调用）
 */
int gui_focus_move(gui_input_t *input, gui_focus_direction_t direction);

/**
 * @brief 设置首次焦点建立策略
 *
 * 初始策略只控制"自动建立首次焦点"，**不会**因为策略变化而偷偷移动已经
 * 存在的选择。
 *
 * @return 0 成功；-1 失败（errno：EINVAL 无效枚举，
 *         EOPNOTSUPP 诊断登记模式，EBUSY 业务回调内调用）
 */
int gui_focus_set_initial_policy(gui_input_t *input, gui_focus_initial_t initial);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_FOCUS_H */
