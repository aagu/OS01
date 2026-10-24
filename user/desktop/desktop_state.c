/*
 * desktop_state.c — 桌面外壳的界面状态机（GUI-3）
 *
 * 纯 C 实现：不包含 LVGL 头，不调用 libc，只有纯粹的迁移规则。
 * 规则真源是设计规范 docs/superpowers/specs/
 * 2026-10-24-gui-3-material-desktop-design.md §5.1。
 */

#include "desktop_state.h"

static bool app_id_is_usable(const char *app_id)
{
    if (app_id == NULL) {
        return false;
    }
    /* 空串同样视为「没有指定应用」。 */
    return app_id[0] != '\0';
}

desktop_state_t desktop_state_next(desktop_state_t cur, desktop_intent_t intent,
                                   const char *app_id)
{
    if (!desktop_state_is_valid(cur)) {
        return DESKTOP_STATE_INVALID;
    }

    switch (intent) {
    case DESKTOP_INTENT_OPEN_DRAWER:
        /* 只有主屏能打开抽屉；抽屉里不再套抽屉。 */
        if (cur == DESKTOP_STATE_HOME) {
            return DESKTOP_STATE_DRAWER;
        }
        return DESKTOP_STATE_INVALID;

    case DESKTOP_INTENT_OPEN_SETTINGS:
        if (cur == DESKTOP_STATE_HOME) {
            return DESKTOP_STATE_SETTINGS;
        }
        return DESKTOP_STATE_INVALID;

    case DESKTOP_INTENT_OPEN_APP:
        /* 只有主屏能直接启动应用；应用表面不允许叠加。 */
        if (cur == DESKTOP_STATE_HOME && app_id_is_usable(app_id)) {
            return DESKTOP_STATE_APP;
        }
        return DESKTOP_STATE_INVALID;

    case DESKTOP_INTENT_BACK:
    case DESKTOP_INTENT_ESC:
        /* 返回上一层：抽屉 / 设置 / 应用一律回主屏；主屏上无副作用。 */
        switch (cur) {
        case DESKTOP_STATE_HOME:
            return DESKTOP_STATE_HOME;
        case DESKTOP_STATE_DRAWER:
        case DESKTOP_STATE_SETTINGS:
        case DESKTOP_STATE_APP:
            return DESKTOP_STATE_HOME;
        default:
            return DESKTOP_STATE_INVALID;
        }

    case DESKTOP_INTENT_NONE:
        /* 没有意图就是没有迁移：状态保持不变。 */
        return cur;

    default:
        return DESKTOP_STATE_INVALID;
    }
}

bool desktop_state_is_valid(desktop_state_t st)
{
    switch (st) {
    case DESKTOP_STATE_HOME:
    case DESKTOP_STATE_DRAWER:
    case DESKTOP_STATE_SETTINGS:
    case DESKTOP_STATE_APP:
        return true;
    default:
        return false;
    }
}

const char *desktop_state_name(desktop_state_t st)
{
    switch (st) {
    case DESKTOP_STATE_HOME:     return "HOME";
    case DESKTOP_STATE_DRAWER:   return "DRAWER";
    case DESKTOP_STATE_SETTINGS: return "SETTINGS";
    case DESKTOP_STATE_APP:      return "APP";
    default:                     return "INVALID";
    }
}
