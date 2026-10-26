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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 离屏冒烟模式的分辨率（仅供自动化测试取像素）。 */
#define SMOKE_WIDTH  640
#define SMOKE_HEIGHT 480

static int write_probe_state(const char *nonce, uint64_t seq, desktop_shell_t *shell)
{
    desktop_probe_state_t st;
    if (desktop_shell_probe_get_state(shell, &st) != 0) {
        return -1;
    }
    char tmp_path[128];
    char target_path[128];
    char buf[1024];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/gui-desktop-%s.state.tmp", nonce);
    snprintf(target_path, sizeof(target_path), "/tmp/gui-desktop-%s.state", nonce);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return -1;
    }
    int len = snprintf(buf, sizeof(buf),
                       "{\n"
                       "  \"nonce\": \"%s\",\n"
                       "  \"seq\": %llu,\n"
                       "  \"surface\": \"%s\",\n"
                       "  \"app_id\": \"%s\",\n"
                       "  \"cwd\": \"%s\",\n"
                       "  \"subtitle\": \"%s\",\n"
                       "  \"focus_tag\": \"%s\",\n"
                       "  \"item_count\": %u\n"
                       "}\n",
                       nonce,
                       (unsigned long long)seq,
                       st.surface,
                       st.app_id,
                       st.cwd,
                       st.subtitle,
                       st.focus_tag,
                       st.item_count);
    if (len > 0) {
        (void)write(fd, buf, (size_t)len);
    }
    close(fd);

    return rename(tmp_path, target_path);
}

static bool probe_state_equal(const desktop_probe_state_t *a,
                              const desktop_probe_state_t *b)
{
    if (a->item_count != b->item_count) return false;
    if (strcmp(a->surface, b->surface) != 0) return false;
    if (strcmp(a->app_id, b->app_id) != 0) return false;
    if (strcmp(a->cwd, b->cwd) != 0) return false;
    if (strcmp(a->subtitle, b->subtitle) != 0) return false;
    if (strcmp(a->focus_tag, b->focus_tag) != 0) return false;
    return true;
}

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
    bool probe = false;
    const char *probe_nonce = NULL;
    if (argc >= 2 && argv[1] != NULL && strcmp(argv[1], "probe") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "desktop: probe requires a hex nonce argument\n");
            return 1;
        }
        probe_nonce = argv[2];
        size_t nlen = strlen(probe_nonce);
        if (nlen == 0 || nlen > 64) {
            fprintf(stderr, "desktop: invalid probe nonce length\n");
            return 1;
        }
        for (size_t i = 0; i < nlen; i++) {
            char c = probe_nonce[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                fprintf(stderr, "desktop: invalid probe nonce (hex required)\n");
                return 1;
            }
        }
        probe = true;
    }

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

    uint64_t probe_seq = 1;
    desktop_probe_state_t last_probe_st;
    memset(&last_probe_st, 0, sizeof(last_probe_st));
    if (probe) {
        desktop_shell_probe_get_state(shell, &last_probe_st);
        write_probe_state(probe_nonce, probe_seq, shell);
    }

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

        /*
         * 迁移只在这里发生：LVGL 回调期间不得销毁控件树。
         *
         * 导航的提交阶段失败（控件创建或焦点登记失败）时，旧树已经被
         * 删除，无法回滚：主循环在这里清理并退出，不假装一切正常。
         */
        if (desktop_shell_apply_pending(shell) != 0) {
            fprintf(stderr, "desktop: the desktop shell failed to rebuild (errno=%d)\n",
                    errno);
            desktop_shell_destroy(shell);
            gui_input_destroy(input);
            gui_platform_destroy(platform);
            return 1;
        }
        desktop_shell_tick(shell);

        if (probe) {
            desktop_probe_state_t cur_probe_st;
            if (desktop_shell_probe_get_state(shell, &cur_probe_st) == 0) {
                if (!probe_state_equal(&cur_probe_st, &last_probe_st)) {
                    probe_seq++;
                    last_probe_st = cur_probe_st;
                    write_probe_state(probe_nonce, probe_seq, shell);
                }
            }
        }

        if (smoke) {
            /* 冒烟模式：渲染一帧后退出，供自动化测试取像素。 */
            break;
        }
    }

    desktop_shell_destroy(shell);
    gui_input_destroy(input);
    gui_platform_destroy(platform);
    return rc;
}
