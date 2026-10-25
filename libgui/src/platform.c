#include <gui/gui_platform.h>
#include <gui_input_private.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>
#include <gui_theme_private.h>
#include <gui_tick.h>
#include <gui_widget_private.h>
#include <gfx.h>
#include "gfx_client_policy.h"
#include <uapi/fb.h>
#include <src/display/lv_display_private.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* struct gui_platform 迁移到 libgui/internal/gui_platform_private.h：
 * 字段原样保留，只新增 `gui_input_t *input`。 */

static gui_platform_t *g_active_platform = NULL;

/* ── 平台内部辅助（供 libgui/src/input.c 使用） ─────────────────── */

bool gui_platform_alive(const gui_platform_t *platform)
{
    if (!platform) {
        return false;
    }
    return platform->terminal_status == GUI_STATUS_OK;
}

bool gui_platform_input_attached(const gui_platform_t *platform)
{
    if (!platform) {
        return false;
    }
    return platform->input != NULL;
}

void gui_platform_attach_input(gui_platform_t *platform, gui_input_t *input)
{
    if (!platform) {
        return;
    }
    platform->input = input;
}

void gui_platform_detach_input(gui_platform_t *platform, gui_input_t *input)
{
    if (!platform) {
        return;
    }
    if (platform->input == input) {
        platform->input = NULL;
    }
}

gfx_handle_t *gui_platform_gfx_handle(gui_platform_t *platform)
{
    if (!platform) {
        return NULL;
    }
    return platform->gfx;
}

int gui_platform_acquire_view(gui_platform_t *platform, bool *acquired_here)
{
    if (!platform) {
        errno = EINVAL;
        return -1;
    }
    if (acquired_here) {
        *acquired_here = false;
    }
    if (!platform->gfx) {
        errno = EINVAL;
        return -1;
    }
    if (platform->acquired) {
        /* 平台已经持有显示所有权：本次调用不取得所有权。 */
        return 0;
    }

    errno = 0;
    if (gfx_acquire(platform->gfx) != 0) {
        errno = errno ? errno : EIO;
        return -1;
    }
    platform->acquired = true;
    if (acquired_here) {
        *acquired_here = true;
    }
    return 0;
}

bool gui_platform_checked_mul_size(size_t a, size_t b, size_t *out)
{
#if defined(__has_builtin) && __has_builtin(__builtin_mul_overflow)
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
#endif
}

static void platform_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    gui_platform_t *platform = (gui_platform_t *)lv_display_get_user_data(disp);
    if (!platform) {
        lv_display_flush_ready(disp);
        return;
    }

    if (!area || !px_map ||
        area->x1 < 0 || area->y1 < 0 ||
        area->x1 > area->x2 || area->y1 > area->y2 ||
        (uint32_t)area->x2 >= platform->width ||
        (uint32_t)area->y2 >= platform->height) {
        platform->flush_error = EINVAL;
        lv_display_flush_ready(disp);
        return;
    }

    int32_t area_w = area->x2 - area->x1 + 1;
    int32_t area_h = area->y2 - area->y1 + 1;
    size_t stride_bytes = (size_t)area_w * sizeof(uint32_t);
    size_t total_bytes = stride_bytes * (size_t)area_h;

    if (total_bytes > platform->draw_buf_bytes) {
        platform->flush_error = EINVAL;
        lv_display_flush_ready(disp);
        return;
    }

    if (platform->mode == GUI_MODE_ONSCREEN) {
        if (platform->gfx) {
            gfx_sprite_blit(platform->gfx, area->x1, area->y1,
                            (const uint32_t *)px_map, (uint32_t)stride_bytes,
                            (uint32_t)area_w, (uint32_t)area_h, false, 0);
            platform->pending_present = true;
        }
    } else {
        const uint32_t *src = (const uint32_t *)px_map;
        for (int32_t row = 0; row < area_h; row++) {
            size_t dst_offset = ((size_t)area->y1 + (size_t)row) * (size_t)platform->width + (size_t)area->x1;
            size_t src_offset = (size_t)row * (size_t)area_w;
            memcpy(&platform->shadow_canvas[dst_offset], &src[src_offset], stride_bytes);
        }
        platform->pending_present = true;
    }

    platform->flush_count++;
    lv_display_flush_ready(disp);
}

gui_platform_t *gui_platform_create(const gui_config_t *cfg)
{
    /* 1. Single instance check */
    if (g_active_platform != NULL) {
        errno = EBUSY;
        return NULL;
    }
    gui_tick_reset();

    gui_mode_t mode = GUI_MODE_ONSCREEN;
    uint32_t req_w = 0;
    uint32_t req_h = 0;
    uint32_t req_lines = 0;

    if (cfg != NULL) {
        mode = cfg->mode;
        req_w = cfg->width;
        req_h = cfg->height;
        req_lines = cfg->buffer_lines;
    }

    if (mode != GUI_MODE_ONSCREEN && mode != GUI_MODE_OFFSCREEN) {
        errno = EINVAL;
        return NULL;
    }

    if (mode == GUI_MODE_ONSCREEN) {
        if (req_w != 0 || req_h != 0) {
            errno = EINVAL;
            return NULL;
        }

        int fb_fd = open("/dev/fb", O_RDONLY);
        if (fb_fd < 0) {
            return NULL;
        }

        struct fb_info fb;
        ssize_t n = read(fb_fd, &fb, sizeof(fb));
        int saved_read_err = errno;
        close(fb_fd);

        if (n < 0) {
            errno = saved_read_err;
            return NULL;
        }
        if ((size_t)n < sizeof(fb) || fb.width == 0 || fb.height == 0 || fb.format != FB_FORMAT_RGB32) {
            errno = EINVAL;
            return NULL;
        }

        req_w = fb.width;
        req_h = fb.height;
    } else {
        if (req_w == 0 || req_h == 0) {
            errno = EINVAL;
            return NULL;
        }
    }

    /* 2. Bounds check */
    if (req_w > 16383 || req_h > 65535) {
        errno = EOVERFLOW;
        return NULL;
    }

    uint32_t lines = req_lines ? req_lines : 40;
    if (lines > req_h) {
        lines = req_h;
    }
    if (lines > UINT16_MAX) {
        errno = EOVERFLOW;
        return NULL;
    }

    size_t stride = 0;
    if (!gui_platform_checked_mul_size((size_t)req_w, sizeof(uint32_t), &stride)) {
        errno = EOVERFLOW;
        return NULL;
    }

    size_t draw_bytes = 0;
    if (!gui_platform_checked_mul_size(stride, (size_t)lines, &draw_bytes)) {
        errno = EOVERFLOW;
        return NULL;
    }
    if (draw_bytes == 0 || draw_bytes > UINT32_MAX) {
        errno = EOVERFLOW;
        return NULL;
    }

    size_t canvas_bytes = 0;
    if (mode == GUI_MODE_OFFSCREEN) {
        if (!gui_platform_checked_mul_size(stride, (size_t)req_h, &canvas_bytes)) {
            errno = EOVERFLOW;
            return NULL;
        }
        if (canvas_bytes == 0) {
            errno = EOVERFLOW;
            return NULL;
        }
    }

    /* 3. Clock availability */
    uint64_t init_now = 0;
    if (gui_tick_now_ms(&init_now) != 0) {
        return NULL;
    }

    /* 4. Allocate platform struct */
    gui_platform_t *platform = (gui_platform_t *)calloc(1, sizeof(gui_platform_t));
    if (!platform) {
        errno = ENOMEM;
        return NULL;
    }

    platform->mode = mode;
    platform->width = req_w;
    platform->height = req_h;
    platform->buffer_lines = lines;
    platform->draw_buf = (uint32_t *)MAP_FAILED;
    platform->shadow_canvas = (uint32_t *)MAP_FAILED;

    int saved = 0;
    bool lv_inited = false;

    /* 5. Screen mode gfx initialization */
    if (mode == GUI_MODE_ONSCREEN) {
        platform->gfx = gfx_open(0, 0, req_w, req_h);
        if (!platform->gfx) {
            saved = errno;
            goto rollback;
        }

        gfx_info_t info = gfx_get_info(platform->gfx);
        if (info.width != req_w || info.height != req_h ||
            info.stride != (uint32_t)stride || info.format != GFX_FORMAT_RGB32) {
            saved = EINVAL;
            goto rollback;
        }
    }

    /* 6. Allocate memory mappings */
    platform->draw_buf = (uint32_t *)mmap(NULL, draw_bytes, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (platform->draw_buf == MAP_FAILED) {
        saved = errno;
        goto rollback;
    }
    platform->draw_buf_bytes = draw_bytes;

    if (mode == GUI_MODE_OFFSCREEN) {
        platform->shadow_canvas = (uint32_t *)mmap(NULL, canvas_bytes, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (platform->shadow_canvas == MAP_FAILED) {
            saved = errno;
            goto rollback;
        }
        platform->shadow_canvas_bytes = canvas_bytes;
        memset(platform->shadow_canvas, 0, canvas_bytes);
    }

    /* 7. Initialize LVGL */
    lv_init();
    lv_inited = true;
    lv_tick_set_cb(gui_tick_get_cb);

    platform->disp = lv_display_create((int32_t)req_w, (int32_t)req_h);
    if (!platform->disp) {
        saved = ENOMEM;
        goto rollback;
    }

    lv_display_set_color_format(platform->disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_user_data(platform->disp, platform);
    lv_display_set_flush_cb(platform->disp, platform_flush_cb);
    lv_display_set_buffers(platform->disp, platform->draw_buf, NULL,
                           (uint32_t)draw_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* GUI-3：平台一律以 Material 设计系统出场。主题落地是 libgui 的
     * 内部职责（令牌表 → LVGL 主题），业务应用不需要也不能自己调用，
     * 因为已发布的公共头不得出现 LVGL 符号。
     * 主题与平台同生共死：样式缓冲在 LVGL 内存池里，必须在 lv_deinit()
     * 之前释放。 */
    platform->theme = gui_theme_create(platform->disp);
    if (platform->theme == NULL) {
        saved = ENOMEM;
        goto rollback;
    }

#ifdef OS01_HOST_TEST
    extern bool g_gui_test_tamper_header;
    if (g_gui_test_tamper_header && platform->disp->buf_1) {
        platform->disp->buf_1->header.stride = 0xbeef;
    }
#endif

    /* GUI-2b：原生控件注册表随平台存在。它由平台 calloc 出来，不依赖
     * 后端内存池（池在平台销毁时被整体释放，注册表必须活得比它久）。
     * 控件创建在平台对外可见之后才可能发生，所以这里建立注册表正好。 */
    if (gui_widget_registry_init(platform) != 0) {
        saved = errno ? errno : ENOMEM;
        goto rollback;
    }

    if (!platform->disp->buf_1 ||
        platform->disp->buf_1->header.w != req_w ||
        platform->disp->buf_1->header.h != lines ||
        platform->disp->buf_1->header.stride != (uint32_t)stride) {
        saved = EINVAL;
        goto rollback;
    }

    g_active_platform = platform;
    return platform;

rollback:
    if (platform->disp) {
        lv_display_delete(platform->disp);
        platform->disp = NULL;
    }
    /* GUI-2b：注册表在 display 之后释放（与正常销毁路径同序）。 */
    gui_widget_registry_fini(platform);
    if (lv_inited) {
        lv_deinit();
    }
    if (platform->shadow_canvas != MAP_FAILED && platform->shadow_canvas_bytes > 0) {
        munmap(platform->shadow_canvas, platform->shadow_canvas_bytes);
        platform->shadow_canvas = (uint32_t *)MAP_FAILED;
    }
    if (platform->draw_buf != MAP_FAILED && platform->draw_buf_bytes > 0) {
        munmap(platform->draw_buf, platform->draw_buf_bytes);
        platform->draw_buf = (uint32_t *)MAP_FAILED;
    }
    if (platform->gfx) {
        if (platform->acquired) {
            gfx_release(platform->gfx);
        }
        gfx_close(platform->gfx);
        platform->gfx = NULL;
    }
    free(platform);
    errno = saved;
    return NULL;
}

void gui_platform_destroy(gui_platform_t *platform)
{
    if (!platform) return;

    /*
     * 仍挂接的输入先销毁：它的 indev 对象还需要活着的 display。
     *
     * 所有权转移：gui_input_create() 挂接成功后，输入对象的**所有权归
     * 平台**——本函数会销毁并释放它，调用方此后不得再使用该指针，也不得
     * 再调用 gui_input_destroy()（否则重复释放）。调用方若要自己掌控
     * 生命周期，必须在销毁平台**之前**先调用 gui_input_destroy()，
     * 那会把输入从平台上摘除，平台随后不再持有它。
     */
    if (platform->input) {
        gui_input_destroy(platform->input);   /* 自行解除挂接 */
    }

    /*
     * 销毁顺序有讲究：**先删 display，再拆主题**。
     *
     * 主题的样式缓冲在 LVGL 内存池里，而 display 的屏幕对象上还挂着这些
     * 样式。lv_display_delete() 会删掉屏幕并摘掉它们的样式；若在这之前
     * 就把主题释放掉，屏幕删除过程会读到已释放的样式（use-after-free），
     * 且症状是偶发的堆破坏。
     *
     * 主题必须在 lv_deinit() 之前释放：lv_deinit() 会把整个 LVGL 内存池
     * 释放掉，池没了之后再去 free 样式就是 double free。
     */
    if (platform->disp) {
        lv_display_delete(platform->disp);
        platform->disp = NULL;
    }

    /* GUI-2b：原生控件树随 display 一起消失（原生删除回调把身份立即置为
     * 失效），注册表随后释放——它必须比 display 活得久，因为删除回调
     * 还要用它。 */
    gui_widget_registry_fini(platform);

    if (platform->theme) {
        gui_theme_destroy(platform->theme);
        platform->theme = NULL;
    }

    lv_deinit();

    if (platform->gfx) {
        if (platform->acquired) {
            gfx_release(platform->gfx);
            platform->acquired = false;
        }
        gfx_close(platform->gfx);
        platform->gfx = NULL;
    }

    if (platform->shadow_canvas != MAP_FAILED && platform->shadow_canvas_bytes > 0) {
        munmap(platform->shadow_canvas, platform->shadow_canvas_bytes);
        platform->shadow_canvas = (uint32_t *)MAP_FAILED;
    }
    if (platform->draw_buf != MAP_FAILED && platform->draw_buf_bytes > 0) {
        munmap(platform->draw_buf, platform->draw_buf_bytes);
        platform->draw_buf = (uint32_t *)MAP_FAILED;
    }

    if (g_active_platform == platform) {
        g_active_platform = NULL;
    }
    gui_tick_reset();
    free(platform);
}

uint32_t gui_platform_get_width(const gui_platform_t *platform)
{
    if (!platform) {
        errno = EINVAL;
        return 0;
    }
    return platform->width;
}

uint32_t gui_platform_get_height(const gui_platform_t *platform)
{
    if (!platform) {
        errno = EINVAL;
        return 0;
    }
    return platform->height;
}

gui_mode_t gui_platform_get_mode(const gui_platform_t *platform)
{
    if (!platform) {
        errno = EINVAL;
        return GUI_MODE_ONSCREEN;
    }
    return platform->mode;
}

lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform)
{
    if (!platform) {
        errno = EINVAL;
        return NULL;
    }
    return platform->disp;
}

uint32_t gui_platform_get_pixel(const gui_platform_t *platform, uint32_t x, uint32_t y)
{
    if (!platform || platform->mode != GUI_MODE_OFFSCREEN) {
        errno = EINVAL;
        return 0xFFFFFFFFu;
    }
    if (x >= platform->width || y >= platform->height) {
        errno = EINVAL;
        return 0xFFFFFFFFu;
    }
    size_t offset = (size_t)y * (size_t)platform->width + (size_t)x;
    return platform->shadow_canvas[offset] & 0x00FFFFFFu;
}

static gui_status_t enter_terminal_state(gui_platform_t *platform, gui_status_t status, int err)
{
    int saved_errno = err;
    /* 输入先停止（释放租约与 fd），再释放显示。 */
    if (platform->input) {
        gui_input_stop(platform->input, saved_errno);
    }
    if (platform->gfx) {
        if (platform->acquired) {
            gfx_release(platform->gfx);
            platform->acquired = false;
        }
        gfx_close(platform->gfx);
        platform->gfx = NULL;
    }
    platform->pending_present = false;
    platform->retry_deadline_ms = 0;
    platform->terminal_status = status;
    platform->terminal_errno = saved_errno;
    errno = saved_errno;
    return status;
}

static gui_status_t compute_wait_and_return(gui_platform_t *platform, uint32_t lv_delay,
                                            uint64_t t_before_handler, gui_status_t status,
                                            uint32_t input_delay_ms, uint32_t *out_next_delay_ms)
{
    uint64_t now = 0;
    if (gui_tick_now_ms(&now) != 0) {
        return enter_terminal_state(platform, GUI_STATUS_ERROR, errno);
    }

    uint32_t wait_ms = 100;
    if (lv_delay != UINT32_MAX /* LV_NO_TIMER_READY */) {
        uint64_t elapsed = now >= t_before_handler ? (now - t_before_handler) : 0;
        if (lv_delay > elapsed) {
            uint32_t remaining = lv_delay - (uint32_t)elapsed;
            wait_ms = remaining > 100 ? 100 : remaining;
        } else {
            wait_ms = 0;
        }
    }

    /* 输入的等待上限（FIFO 余项 / 前缀截止）参与取最小值。 */
    if (input_delay_ms != UINT32_MAX && input_delay_ms < wait_ms) {
        wait_ms = input_delay_ms;
    }

    if (platform->pending_present && platform->retry_deadline_ms != 0) {
        if (now >= platform->retry_deadline_ms) {
            wait_ms = 0;
        } else {
            uint64_t rem = platform->retry_deadline_ms - now;
            uint32_t rem_ms = rem > 100 ? 100 : (uint32_t)rem;
            if (rem_ms < wait_ms) {
                wait_ms = rem_ms;
            }
        }
    }

    if (out_next_delay_ms) {
        *out_next_delay_ms = wait_ms;
    }
    return status;
}

gui_status_t gui_platform_step(gui_platform_t *platform, uint32_t *out_next_delay_ms)
{
    if (out_next_delay_ms) {
        *out_next_delay_ms = 100;
    }

    if (!platform) {
        errno = EINVAL;
        return GUI_STATUS_ERROR;
    }

    if (platform->terminal_status != GUI_STATUS_OK) {
        errno = platform->terminal_errno;
        return platform->terminal_status;
    }

    if (gui_tick_error() != 0) {
        int err = gui_tick_error();
        return enter_terminal_state(platform, GUI_STATUS_ERROR, err);
    }

    uint64_t t_before_handler = 0;
    if (gui_tick_now_ms(&t_before_handler) != 0) {
        return enter_terminal_state(platform, GUI_STATUS_ERROR, errno);
    }

    /*
     * GUI-2：先检查显示所有权与已锁存的输入错误，再投递 FIFO。
     * 健康的队列**不会**在 step 里被偷读——设备读取只发生在
     * gui_input_dispatch() 里。没有输入时本段完全不执行，GUI-1 的
     * 顺序保持原样。
     */
    uint32_t input_delay_ms = UINT32_MAX;
    if (platform->input) {
        gui_status_t input_status = gui_input_advance(platform->input, t_before_handler,
                                                      &input_delay_ms);
        if (input_status != GUI_STATUS_OK) {
            int err = errno ? errno : EIO;
            return enter_terminal_state(platform, input_status, err);
        }
    }

    uint32_t lv_delay = lv_timer_handler();

    if (gui_tick_error() != 0) {
        int err = gui_tick_error();
        return enter_terminal_state(platform, GUI_STATUS_ERROR, err);
    }
    if (platform->flush_error != 0) {
        int err = platform->flush_error;
        return enter_terminal_state(platform, GUI_STATUS_ERROR, err);
    }

    if (platform->mode == GUI_MODE_OFFSCREEN) {
        platform->pending_present = false;
        return compute_wait_and_return(platform, lv_delay, t_before_handler, GUI_STATUS_OK,
                                         input_delay_ms, out_next_delay_ms);
    }

    gui_status_t ret_status = GUI_STATUS_OK;

    if (platform->pending_present) {
        uint64_t now = 0;
        if (gui_tick_now_ms(&now) != 0) {
            return enter_terminal_state(platform, GUI_STATUS_ERROR, errno);
        }

        if (platform->retry_deadline_ms != 0 && now < platform->retry_deadline_ms) {
            ret_status = GUI_STATUS_EAGAIN;
        } else {
            if (!platform->acquired) {
                errno = 0;
                if (gfx_acquire(platform->gfx) != 0) {
                    int err = errno ? errno : EIO;
                    return enter_terminal_state(platform, GUI_STATUS_ERROR, err);
                }
                platform->acquired = true;
            }

            errno = 0;
            if (gfx_present(platform->gfx) == 0) {
                platform->pending_present = false;
                platform->retry_deadline_ms = 0;
                ret_status = GUI_STATUS_OK;
            } else {
                int err = errno ? errno : EIO;
                if (gfx_client_present_policy(err) == 1) { /* EAGAIN */
                    uint64_t now_after = 0;
                    if (gui_tick_now_ms(&now_after) != 0) {
                        return enter_terminal_state(platform, GUI_STATUS_ERROR, errno);
                    }
                    platform->retry_deadline_ms = now_after + GFX_CLIENT_RETRY_MS;
                    ret_status = GUI_STATUS_EAGAIN;
                } else if (err == EACCES) {
                    return enter_terminal_state(platform, GUI_STATUS_LOST_OWNERSHIP, EACCES);
                } else if (err == ESTALE) {
                    return enter_terminal_state(platform, GUI_STATUS_DISPLAY_CHANGED, ESTALE);
                } else {
                    return enter_terminal_state(platform, GUI_STATUS_ERROR, err);
                }
            }
        }
    }

    return compute_wait_and_return(platform, lv_delay, t_before_handler, ret_status,
                                     input_delay_ms, out_next_delay_ms);
}
