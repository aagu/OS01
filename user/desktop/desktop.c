/*
 * desktop.c — OS01 Material 桌面外壳主入口（GUI-3）
 *
 * 这一版把桌面从「手绘像素 + 自读设备文件」彻底换成
 * **libgui 平台 + libgui 输入 + LVGL 界面**：
 *
 *   - 画面由 LVGL 渲染，样式全部来自 libgui 的 Material 令牌表；
 *   - 键鼠一律走 GUI-2 的输入通道（独占租约 + LVGL indev + 焦点组），
 *     本文件**不再** open("/dev/keyboard") 或 open("/dev/mouse")；
 *   - 界面状态由 user/desktop/desktop_state.c 的状态机驱动，迁移一律是
 *     整屏替换：旧表面的控件树销毁，新表面从零构建。
 *
 * 明确不做：不实现窗口系统，因此**不绘制任何窗口装饰**（无标题栏边框、
 * 无最小化 / 最大化 / 关闭按钮、无 Z-order）。应用表面恒为全屏。
 */

#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>

#include "desktop_shell.h"
#include "desktop_state.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 离屏冒烟模式的分辨率（仅供自动化测试取像素）。 */
#define SMOKE_WIDTH  640
#define SMOKE_HEIGHT 480

static void report_platform_failure(const char *what, int err)
{
    if (err == EBUSY) {
        fprintf(stderr, "desktop: %s: the display is owned by another GUI\n", what);
    } else {
        fprintf(stderr, "desktop: %s: %s (errno=%d)\n", what, strerror(err), err);
    }
}

/*
 * 原生按键回调：先于 LVGL 控件路径执行。
 *
 * 返回 true 表示本次按键已被外壳消费，不再进入 LVGL 的 keypad 路径。
 * ESC 交给外壳做「返回上一层」；其余键（Tab / Enter / 方向键）一律
 * 交给 LVGL 的焦点组与控件处理。
 */
static bool on_native_key(const gui_key_event_t *event, void *userdata)
{
    desktop_shell_t *shell = (desktop_shell_t *)userdata;
    if (shell == NULL || event == NULL) {
        return false;
    }
    return desktop_shell_handle_key(shell, event);
}

int main(int argc, char **argv)
{
    bool smoke = (argc > 1 && argv[1] != NULL && strcmp(argv[1], "smoke") == 0);

    gui_config_t pcfg = {
        .mode = GUI_MODE_ONSCREEN,
        .width = 0,
        .height = 0,
        .buffer_lines = 0,
    };
    if (smoke) {
        pcfg.mode = GUI_MODE_OFFSCREEN;
        pcfg.width = SMOKE_WIDTH;
        pcfg.height = SMOKE_HEIGHT;
    }

    gui_platform_t *platform = gui_platform_create(&pcfg);
    if (platform == NULL) {
        report_platform_failure("cannot create the display platform", errno);
        return 1;
    }

    /*
     * 输入：允许无鼠标降级（纯键盘也能跑），但设备缺失之外的原因仍然是
     * 致命错误。
     *
     * 输入模式必须与平台模式配对（libgui 的硬约束）：真实设备只允许屏幕
     * 平台，合成输入只允许离屏平台。冒烟模式跑在离屏平台上，因此用合成
     * 输入 —— 它不碰 /dev/keyboard，也不占显示租约。
     */
    gui_input_config_t icfg = {
        .mode = smoke ? GUI_INPUT_SYNTHETIC : GUI_INPUT_DEVICES,
        .require_mouse = false,
    };
    gui_input_t *input = gui_input_create(platform, &icfg);
    if (input == NULL) {
        report_platform_failure("cannot create the input", errno);
        gui_platform_destroy(platform);
        return 1;
    }

    /*
     * 外壳需要平台与输入**两个**有效且关联的句柄：焦点引擎住在输入对象
     * 里，外壳登记的每一个可聚焦控件都必须属于同一个平台。
     */
    desktop_shell_t *shell = desktop_shell_create(platform, input);
    if (shell == NULL) {
        fprintf(stderr, "desktop: cannot create the desktop shell (errno=%d)\n",
                errno);
        gui_input_destroy(input);
        gui_platform_destroy(platform);
        return 1;
    }

    gui_input_set_key_handler(input, on_native_key, shell);

    struct pollfd fds[2];
    unsigned nfds = 0;
    int rc = 0;

    for (;;) {
        nfds = 0;
        int want = gui_input_get_pollfds(input, fds);
        if (want > 0) {
            nfds = (unsigned)want;
            int ready = poll(fds, nfds, 50);
            if (ready > 0) {
                gui_input_dispatch(input, fds, nfds);
            } else if (ready < 0 && errno != EINTR) {
                fprintf(stderr, "desktop: poll failed (errno=%d)\n", errno);
                rc = 1;
                break;
            }
        }

        uint32_t next_delay = 0;
        gui_status_t status = gui_platform_step(platform, &next_delay);
        if (status == GUI_STATUS_EAGAIN) {
            continue;
        }
        if (status != GUI_STATUS_OK) {
            fprintf(stderr, "desktop: the display platform reached a terminal state (status=%d, errno=%d)\n",
                    (int)status, errno);
            rc = 1;
            break;
        }

        /* 迁移只在这里发生：LVGL 回调期间不得销毁控件树。 */
        desktop_shell_apply_pending(shell);
        desktop_shell_tick(shell);

        if (smoke) {
            /* 冒烟模式：渲染一帧后退出，供自动化测试取像素。 */
            break;
        }
    }

    desktop_shell_destroy(shell);
    gui_platform_destroy(platform);
    return rc;
}
