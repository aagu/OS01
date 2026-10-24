#ifndef _DESKTOP_STATE_H
#define _DESKTOP_STATE_H

/*
 * desktop_state.h — 桌面外壳的界面状态机（GUI-3）
 *
 * 本头是**纯 C**：不包含任何 LVGL 头，也不调用 libc。理由有两个：
 *   1. 迁移规则是外壳最核心的契约（「应用启动 / 退出 / 全屏切换」的语义
 *      载体），必须能在宿主上被穷举验证，而不是只能在 QEMU 里肉眼判断。
 *   2. LVGL 回调期间销毁控件树是未定义行为，因此迁移必须由主循环串行
 *      执行；把规则抽成不依赖任何全局状态的纯函数，是保证「只有主循环
 *      会执行迁移」的最直接手段。
 *
 * 语义约定（设计规范 §5.1 / §6）：
 *   - 外壳同时只有一个可见表面，所有表面都占满整个显示区（全屏）。
 *   - 状态迁移一律是**整屏替换**：旧表面的控件树被销毁，新表面从零构建。
 *   - 应用不允许叠加：APP 状态下再打开另一个应用是非法迁移。
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 外壳的界面状态
 */
typedef enum {
    DESKTOP_STATE_HOME = 0,      /* 主屏：状态栏 + 欢迎卡片 + 启动器 */
    DESKTOP_STATE_DRAWER,        /* 应用抽屉：全部应用列表 */
    DESKTOP_STATE_SETTINGS,      /* 基本设置 */
    DESKTOP_STATE_APP,           /* 某个应用（全屏应用表面） */
    DESKTOP_STATE_INVALID = -1   /* 非法状态 / 非法迁移结果 */
} desktop_state_t;

/**
 * @brief 外壳能接收的迁移意图
 *
 * 意图由 UI 回调**投递**，由主循环**执行**；回调本身不直接改状态。
 */
typedef enum {
    DESKTOP_INTENT_NONE = 0,       /* 无意图 */
    DESKTOP_INTENT_OPEN_DRAWER,    /* 打开应用抽屉 */
    DESKTOP_INTENT_OPEN_SETTINGS,  /* 打开基本设置 */
    DESKTOP_INTENT_OPEN_APP,       /* 启动某个应用（需带 app_id） */
    DESKTOP_INTENT_BACK,           /* 返回上一层 */
    DESKTOP_INTENT_ESC             /* ESC 键（语义等同返回上一层） */
} desktop_intent_t;

/**
 * @brief 计算一次迁移的结果
 *
 * 纯函数：不读全局状态、不改任何东西。
 *
 * 允许的迁移（设计规范 §5.1）：
 *   HOME    + OPEN_DRAWER   → DRAWER
 *   HOME    + OPEN_SETTINGS → SETTINGS
 *   HOME    + OPEN_APP(id)  → APP
 *   HOME    + BACK / ESC    → HOME（无副作用）
 *   DRAWER  + BACK / ESC    → HOME
 *   SETTINGS+ BACK / ESC    → HOME
 *   APP     + BACK / ESC    → HOME
 *   任意     + NONE         → 原状态（没有意图就是没有迁移）
 *
 * 其余组合一律返回 DESKTOP_STATE_INVALID。特别是 APP + OPEN_APP —— 应用
 * 不允许叠加，必须先回到 HOME。
 *
 * @param cur   当前状态
 * @param intent 迁移意图
 * @param app_id 目标应用标识；仅 OPEN_APP 需要，非空且非空串才有效
 * @return 迁移后的状态；非法迁移返回 DESKTOP_STATE_INVALID
 */
desktop_state_t desktop_state_next(desktop_state_t cur, desktop_intent_t intent,
                                   const char *app_id);

/**
 * @brief 判断状态是否合法
 * @param st 状态
 * @return 合法返回 true
 */
bool desktop_state_is_valid(desktop_state_t st);

/**
 * @brief 取状态的显示名（用于诊断输出）
 * @param st 状态
 * @return 静态字符串；非法状态返回 "INVALID"
 */
const char *desktop_state_name(desktop_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* _DESKTOP_STATE_H */
