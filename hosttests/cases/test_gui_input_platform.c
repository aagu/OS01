/*
 * test_gui_input_platform.c — 生命周期、FIFO、公平泵与平台错误收敛
 *
 * GUI-2 plan Task 6.  The fixture host-compiles the REAL libgui input
 * unit (libgui/src/input.c + libgui/src/input_lvgl.c + the REAL LVGL
 * adapter) and links it against the recording runtime in
 * hosttests/mock/gui_test_runtime.{c,h}.
 *
 * Contract under test (design spec §5.1 / §5.2):
 *
 *   Lifecycle
 *     - the creation order is fixed (alive → open → allocate → LVGL
 *       objects → acquire → bind → attach) and every stage rolls back in
 *       reverse order, keeping the FIRST errno;
 *     - the fd / lease / mapping net counts return to the baseline after
 *       a failed creation;
 *     - the keyboard is required; the mouse degrades to keyboard-only
 *       ONLY for ENOENT / ENODEV and only when require_mouse == false;
 *     - a second create on the same platform is EBUSY and leaves the
 *       first input intact;
 *     - a failed creation that did NOT acquire the view must not release
 *       the platform's pre-existing display ownership;
 *     - an offscreen synthetic input never touches a device;
 *     - only the Caps latch is seeded into the decoder: the aggregate
 *       Shift/Ctrl/Alt bits of the lease snapshot are never planted.
 *
 *   Pump and platform
 *     - a mouse read that is not a multiple of 8 bytes is EIO;
 *     - a short keyboard read is legal and the prefix is carried over;
 *     - EINTR / EAGAIN / the legacy empty read end the round without an
 *       error;
 *     - POLLERR takes the latched reason from one non-blocking read,
 *       POLLHUP is ESTALE and POLLNVAL is EBADF;
 *     - the 512-item FIFO stops the pump instead of overflowing and the
 *       down/up pair is never merged;
 *     - every device has its own per-round budget, a step delivers at
 *       most 256 items and the delay is 0 while items remain;
 *     - a static view that lost the owner stops the delivery even when
 *       nothing was ever flushed;
 *     - the callbacks run inside step only; dispatch never calls LVGL.
 */
#include "test_framework.h"
#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include "gui_test_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/mouse.h>
#include <lvgl/lvgl.h>

/* ── 记录用的回调 ─────────────────────────────────────────────── */

#define MAX_LOGGED 64

typedef struct {
    gui_key_event_t events[MAX_LOGGED];
    int count;
} key_log_t;

typedef struct {
    gui_pointer_event_t events[MAX_LOGGED];
    int count;
} pointer_log_t;

static bool record_key(const gui_key_event_t *event, void *userdata)
{
    key_log_t *log = (key_log_t *)userdata;
    if (log->count < MAX_LOGGED) {
        log->events[log->count] = *event;
    }
    log->count++;
    return false;   /* 不消费：事件继续进入控件路径 */
}

static void record_pointer(const gui_pointer_event_t *event, void *userdata)
{
    pointer_log_t *log = (pointer_log_t *)userdata;
    if (log->count < MAX_LOGGED) {
        log->events[log->count] = *event;
    }
    log->count++;
}

/* ── LVGL hook 的失败注入包装（产品链接仍然是真实 hook） ───────── */

/* platform.c 的 OS01_HOST_TEST 断言注入点：本用例不篡改帧头，但符号必须存在。 */
bool g_gui_test_tamper_header = false;

extern int __real_gui_input_lvgl_create(gui_input_t *input);
int __wrap_gui_input_lvgl_create(gui_input_t *input)
{
    g_gui_test_counts.lvgl_create++;
    if (g_gui_test_fail_lvgl_create_errno) {
        errno = g_gui_test_fail_lvgl_create_errno;
        return -1;
    }
    return __real_gui_input_lvgl_create(input);
}

/* ── 辅助 ─────────────────────────────────────────────────────── */

static lv_indev_t *find_indev(lv_indev_type_t type)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == type) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}

static int count_indevs(void)
{
    int n = 0;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        n++;
        indev = lv_indev_get_next(indev);
    }
    return n;
}

static int count_timers(void)
{
    int n = 0;
    lv_timer_t *timer = lv_timer_get_next(NULL);
    while (timer) {
        n++;
        timer = lv_timer_get_next(timer);
    }
    return n;
}

static void push_mouse(gui_test_device_t *dev, uint8_t buttons, int16_t dx, int16_t dy,
                       int8_t wheel)
{
    mouse_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.buttons = buttons;
    ev.wheel = wheel;
    ev.dx = dx;
    ev.dy = dy;
    gui_test_device_add_data(dev, &ev, sizeof(ev));
}

/* 反复投递同一个 make/break 对，产生 n 个按键边沿事件。 */
static void push_key_pairs(gui_test_device_t *dev, uint8_t make, uint8_t brk, int n)
{
    uint8_t buf[1024];
    size_t len = 0;
    for (int i = 0; i < n && len + 2 <= sizeof(buf); i++) {
        buf[len++] = make;
        buf[len++] = brk;
    }
    gui_test_device_add_data(dev, buf, len);
    gui_test_device_add_op(dev, 0, len);
}

/* ── 1. 生命周期 ──────────────────────────────────────────────── */

TEST_FUNC(test_lifecycle_create_and_destroy)
{
    gui_test_reset();

    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    uint32_t opens_before = g_gui_test_counts.open;
    uint32_t fd_closes_before = g_gui_test_counts.fd_close;
    uint32_t mmaps_before = g_gui_test_counts.mmap;
    uint32_t munmaps_before = g_gui_test_counts.munmap;

    gui_input_config_t cfg = {
        .mode = GUI_INPUT_DEVICES,
        .require_mouse = false,
    };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    /* 两个设备都被打开，且是 O_RDONLY|O_NONBLOCK。 */
    assert_eq(opens_before + 2, g_gui_test_counts.open);
    assert_eq(O_RDONLY | O_NONBLOCK, g_gui_test_input_open_flags);

    /* 租约已绑定。 */
    assert_eq(1, g_gui_test_counts.bind);

    assert_true(gui_input_has_mouse(input));

    struct pollfd fds[2];
    int n = gui_input_get_pollfds(input, fds);
    assert_eq(2, n);
    assert_eq(GUI_TEST_KEYBOARD_FD, fds[0].fd);
    assert_eq(GUI_TEST_MOUSE_FD, fds[1].fd);
    assert_eq(POLLIN, fds[0].events);
    assert_eq(POLLIN, fds[1].events);
    assert_eq(0, fds[0].revents);
    assert_eq(0, fds[1].revents);

    /* 销毁：解绑租约、关闭两个设备。 */
    gui_input_destroy(input);
    assert_eq(1, g_gui_test_counts.unbind);
    assert_eq(fd_closes_before + 2, g_gui_test_counts.fd_close);

    /* 输入对象不使用 mmap：映射净计数不变。 */
    assert_eq(mmaps_before, g_gui_test_counts.mmap);
    assert_eq(munmaps_before, g_gui_test_counts.munmap);

    gui_platform_destroy(platform);
}

TEST_FUNC(test_lifecycle_rollback_each_stage)
{
    /* (a) 打开键盘失败：errno 原样透传，没有 fd 泄漏，没有租约。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        uint32_t opens_before = g_gui_test_counts.open;
        g_gui_test_kbd_open_fails = true;
        g_gui_test_kbd_open_errno = ENOENT;

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(ENOENT, errno);
        assert_eq(opens_before, g_gui_test_counts.open);
        assert_eq(0, g_gui_test_counts.bind);

        g_gui_test_kbd_open_fails = false;
        gui_platform_destroy(platform);
    }

    /* (b) 分配失败：已打开的设备必须被关闭。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        uint32_t opens_before = g_gui_test_counts.open;
        uint32_t closes_before = g_gui_test_counts.fd_close;
        uint32_t mmaps_before = g_gui_test_counts.mmap;
        uint32_t munmaps_before = g_gui_test_counts.munmap;

        g_gui_test_fail_calloc = true;
        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(ENOMEM, errno);
        assert_eq(opens_before + 2, g_gui_test_counts.open);
        assert_eq(closes_before + 2, g_gui_test_counts.fd_close);
        assert_eq(0, g_gui_test_counts.bind);
        assert_eq(mmaps_before, g_gui_test_counts.mmap);
        assert_eq(munmaps_before, g_gui_test_counts.munmap);

        g_gui_test_fail_calloc = false;
        gui_platform_destroy(platform);
    }

    /* (c) acquire 失败：errno 透传，设备关闭，没有租约。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        uint32_t closes_before = g_gui_test_counts.fd_close;
        uint32_t releases_before = g_gui_test_counts.release;
        uint32_t mmaps_before = g_gui_test_counts.mmap;
        uint32_t munmaps_before = g_gui_test_counts.munmap;

        g_gui_test_fail_acquire_errno = EACCES;
        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(EACCES, errno);
        assert_eq(closes_before + 2, g_gui_test_counts.fd_close);
        assert_eq(0, g_gui_test_counts.bind);
        /* 没有取得所有权：不释放平台原有的显示所有权。 */
        assert_eq(releases_before, g_gui_test_counts.release);
        assert_eq(mmaps_before, g_gui_test_counts.mmap);
        assert_eq(munmaps_before, g_gui_test_counts.munmap);

        g_gui_test_fail_acquire_errno = 0;
        gui_platform_destroy(platform);
    }

    /* (d) bind 失败：已经取得的视图必须被 release。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        uint32_t closes_before = g_gui_test_counts.fd_close;
        uint32_t releases_before = g_gui_test_counts.release;
        uint32_t mmaps_before = g_gui_test_counts.mmap;
        uint32_t munmaps_before = g_gui_test_counts.munmap;

        g_gui_test_fail_bind_errno = EBUSY;
        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(EBUSY, errno);
        assert_eq(closes_before + 2, g_gui_test_counts.fd_close);
        assert_eq(0, g_gui_test_counts.bind);
        assert_eq(releases_before + 1, g_gui_test_counts.release);
        assert_eq(mmaps_before, g_gui_test_counts.mmap);
        assert_eq(munmaps_before, g_gui_test_counts.munmap);

        g_gui_test_fail_bind_errno = 0;
        gui_platform_destroy(platform);
    }

    /* (e) LVGL 对象创建失败：发生在 acquire 之前，不释放视图。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        uint32_t closes_before = g_gui_test_counts.fd_close;
        uint32_t releases_before = g_gui_test_counts.release;
        uint32_t mmaps_before = g_gui_test_counts.mmap;
        uint32_t munmaps_before = g_gui_test_counts.munmap;

        g_gui_test_fail_lvgl_create_errno = ENOMEM;
        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(ENOMEM, errno);
        assert_eq(closes_before + 2, g_gui_test_counts.fd_close);
        assert_eq(0, g_gui_test_counts.bind);
        assert_eq(releases_before, g_gui_test_counts.release);
        assert_eq(mmaps_before, g_gui_test_counts.mmap);
        assert_eq(munmaps_before, g_gui_test_counts.munmap);

        g_gui_test_fail_lvgl_create_errno = 0;
        gui_platform_destroy(platform);
    }
}

TEST_FUNC(test_lifecycle_mouse_degradation)
{
    /* ENOENT + require_mouse == false → keyboard-only。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        g_gui_test_mouse_open_fails = true;
        g_gui_test_mouse_open_errno = ENOENT;

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);
        assert_false(gui_input_has_mouse(input));

        struct pollfd fds[2];
        assert_eq(1, gui_input_get_pollfds(input, fds));
        assert_eq(GUI_TEST_KEYBOARD_FD, fds[0].fd);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }

    /* ENODEV + require_mouse == false → keyboard-only。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        g_gui_test_mouse_open_fails = true;
        g_gui_test_mouse_open_errno = ENODEV;

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);
        assert_false(gui_input_has_mouse(input));

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }

    /* 其他错误不得被静默忽略。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        g_gui_test_mouse_open_fails = true;
        g_gui_test_mouse_open_errno = EACCES;

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(EACCES, errno);

        gui_platform_destroy(platform);
    }

    /* require_mouse == true 时鼠标缺失即创建失败。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        g_gui_test_mouse_open_fails = true;
        g_gui_test_mouse_open_errno = ENOENT;

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = true };
        errno = 0;
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_null(input);
        assert_eq(ENOENT, errno);

        gui_platform_destroy(platform);
    }
}

TEST_FUNC(test_lifecycle_second_create_is_ebusy)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *first = gui_input_create(platform, &cfg);
    assert_not_null(first);

    errno = 0;
    gui_input_t *second = gui_input_create(platform, &cfg);
    assert_null(second);
    assert_eq(EBUSY, errno);

    /* 第二次创建不得打扰第一个：租约仍然只有一份，fd 仍然有效。 */
    assert_eq(1, g_gui_test_counts.bind);
    struct pollfd fds[2];
    assert_eq(2, gui_input_get_pollfds(first, fds));

    gui_input_destroy(first);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_lifecycle_synthetic_and_mode_rules)
{
    /* 离屏平台 + 合成输入：不打开任何设备，不占 gfx 租约。 */
    gui_test_reset();
    gui_config_t off = { .mode = GUI_MODE_OFFSCREEN, .width = 320, .height = 200 };
    gui_platform_t *platform = gui_platform_create(&off);
    assert_not_null(platform);

    uint32_t opens_before = g_gui_test_counts.open;
    gui_input_config_t cfg = { .mode = GUI_INPUT_SYNTHETIC, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);
    assert_eq(opens_before, g_gui_test_counts.open);
    assert_eq(0, g_gui_test_counts.bind);
    assert_false(gui_input_has_mouse(input));

    struct pollfd fds[2];
    assert_eq(0, gui_input_get_pollfds(input, fds));
    assert_eq(0, gui_input_dispatch(input, NULL, 0));

    gui_input_destroy(input);
    gui_platform_destroy(platform);

    /* 组合错误：离屏平台不允许真实设备。 */
    {
        gui_test_reset();
        gui_platform_t *off2 = gui_platform_create(&off);
        assert_not_null(off2);
        gui_input_config_t devices = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        errno = 0;
        gui_input_t *bad = gui_input_create(off2, &devices);
        assert_null(bad);
        assert_eq(EINVAL, errno);
        gui_platform_destroy(off2);
    }

    /* 组合错误：屏幕平台不允许合成输入。 */
    {
        gui_test_reset();
        gui_platform_t *on = gui_platform_create(NULL);
        assert_not_null(on);
        gui_input_config_t synthetic = { .mode = GUI_INPUT_SYNTHETIC, .require_mouse = false };
        errno = 0;
        gui_input_t *bad = gui_input_create(on, &synthetic);
        assert_null(bad);
        assert_eq(EINVAL, errno);
        gui_platform_destroy(on);
    }
}

TEST_FUNC(test_lifecycle_failed_create_keeps_previous_owner)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    /* 平台先取得显示所有权（一次失败的 present 会触发 acquire）。 */
    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);
    assert_eq(1, g_gui_test_counts.acquire);
    gui_input_destroy(input);
    /* 销毁输入不释放平台仍拥有的 gfx。 */
    assert_eq(0, g_gui_test_counts.release);

    uint32_t releases_before = g_gui_test_counts.release;

    /* 这次创建在 bind 阶段失败：平台原有的显示所有权不得被释放。 */
    g_gui_test_fail_bind_errno = ESTALE;
    errno = 0;
    gui_input_t *failed = gui_input_create(platform, &cfg);
    assert_null(failed);
    assert_eq(ESTALE, errno);
    assert_eq(releases_before, g_gui_test_counts.release);

    g_gui_test_fail_bind_errno = 0;
    gui_platform_destroy(platform);
}

TEST_FUNC(test_caps_only_seeding)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    /* 租约快照的所有位都是 1：只有 Caps 锁存允许被植入。 */
    g_gui_test_bind_initial_modifiers = 0xFFFFFFFFu;

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    /* 'a' 的 make 字节。 */
    uint8_t make_a = 0x1E;
    gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);

    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(0, log.count);

    uint32_t delay = 0;
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
    assert_eq(1, log.count);

    /* Caps 打开：字母是 Shift XOR Caps → 'A'；Shift 绝不能被植入。 */
    assert_eq('A', log.events[0].codepoint);
    assert_eq(GUI_MOD_CAPS, log.events[0].modifiers);
    assert_eq(0, log.events[0].modifiers & GUI_MOD_LSHIFT);
    assert_eq(0, log.events[0].modifiers & GUI_MOD_RSHIFT);

    gui_input_destroy(input);
    gui_platform_destroy(platform);

    /* 没有 Caps 时同一个字节产生小写字母。 */
    gui_test_reset();
    platform = gui_platform_create(NULL);
    assert_not_null(platform);

    g_gui_test_bind_initial_modifiers = 0;
    input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);

    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
    assert_eq(1, log.count);
    assert_eq('a', log.events[0].codepoint);
    assert_eq(0, log.events[0].modifiers);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

/* ── 2. 泵与平台 ──────────────────────────────────────────────── */

TEST_FUNC(test_pump_read_errors)
{
    /* 鼠标读出 7 字节（不是 8 的倍数）→ EIO。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);

        uint8_t junk[7];
        memset(junk, 0, sizeof(junk));
        gui_test_device_add_data(&g_gui_test_mouse, junk, sizeof(junk));
        gui_test_device_add_op(&g_gui_test_mouse, 0, sizeof(junk));

        errno = 0;
        assert_eq(-1, gui_input_dispatch(input, NULL, 0));
        assert_eq(EIO, errno);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }

    /* EINTR / EAGAIN / 旧路径的空读 0：都不算错误。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);

        /* 第一次读被中断：不是错误（errno 在成功之后无意义）。 */
        gui_test_device_add_op(&g_gui_test_keyboard, EINTR, 0);
        assert_eq(0, gui_input_dispatch(input, NULL, 0));

        /* 第二次读没有数据（EAGAIN）。 */
        gui_test_device_add_op(&g_gui_test_keyboard, EAGAIN, 0);
        assert_eq(0, gui_input_dispatch(input, NULL, 0));

        /* 第三次是旧 NORMAL 路径的空读 0：不是永久 EOF。 */
        gui_test_device_add_op(&g_gui_test_keyboard, 0, 0);
        assert_eq(0, gui_input_dispatch(input, NULL, 0));

        /* 之后仍然可以读到数据：空读没有把设备当作 EOF。 */
        uint8_t make_a = 0x1E;
        gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
        gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);

        key_log_t log;
        memset(&log, 0, sizeof(log));
        gui_input_set_key_handler(input, record_key, &log);

        assert_eq(0, gui_input_dispatch(input, NULL, 0));
        uint32_t delay = 0;
        assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
        assert_eq(1, log.count);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }
}

TEST_FUNC(test_pump_short_read_carries_prefix)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    /* E0 前缀与 1D 分两次到达：前缀必须被保留。 */
    uint8_t prefix = 0xE0;
    uint8_t ctrl = 0x1D;
    gui_test_device_add_data(&g_gui_test_keyboard, &prefix, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);
    gui_test_device_add_data(&g_gui_test_keyboard, &ctrl, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    uint32_t delay = 0;
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));

    assert_eq(1, log.count);
    assert_eq(GUI_KEY_DOWN, log.events[0].action);
    assert_eq(0x11D, log.events[0].physical_key);   /* E0 1D = 右 Ctrl */
    assert_eq(GUI_MOD_RCTRL, log.events[0].modifiers);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_pump_poll_flags_map_to_errors)
{
    /* POLLERR → 一次非阻塞读取得内核锁存的原因（EOVERFLOW）。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);

        struct pollfd fds[2];
        int n = gui_input_get_pollfds(input, fds);
        assert_eq(2, n);
        fds[0].revents = POLLERR;

        gui_test_device_add_op(&g_gui_test_keyboard, EOVERFLOW, 0);

        errno = 0;
        assert_eq(-1, gui_input_dispatch(input, fds, n));
        assert_eq(EOVERFLOW, errno);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }

    /* POLLHUP → ESTALE。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);

        struct pollfd fds[2];
        int n = gui_input_get_pollfds(input, fds);
        fds[0].revents = POLLHUP;

        errno = 0;
        assert_eq(-1, gui_input_dispatch(input, fds, n));
        assert_eq(ESTALE, errno);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }

    /* POLLNVAL → 直接 EBADF（不依赖 poll 标志的区分度）。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);

        struct pollfd fds[2];
        int n = gui_input_get_pollfds(input, fds);
        fds[0].revents = POLLNVAL;

        errno = 0;
        assert_eq(-1, gui_input_dispatch(input, fds, n));
        assert_eq(EBADF, errno);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }

    /* 不是本对象的 fd → EINVAL。 */
    {
        gui_test_reset();
        gui_platform_t *platform = gui_platform_create(NULL);
        assert_not_null(platform);

        gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);

        struct pollfd fds[2];
        fds[0].fd = 4242;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        errno = 0;
        assert_eq(-1, gui_input_dispatch(input, fds, 1));
        assert_eq(EINVAL, errno);

        gui_input_destroy(input);
        gui_platform_destroy(platform);
    }
}

TEST_FUNC(test_pump_budgets_and_fifo_capacity)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    /* 1024 字节 = 512 个边沿事件。 */
    push_key_pairs(&g_gui_test_keyboard, 0x1E, 0x9E, 512);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    /* 第一轮：键盘预算 256 字节。 */
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(256, gui_test_device_consumed(&g_gui_test_keyboard));

    /* 第二轮：再读 256 字节，FIFO 达到 512 项。 */
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(512, gui_test_device_consumed(&g_gui_test_keyboard));

    /* 第三轮：FIFO 已满 → 停止读取，不产生假溢出。 */
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(512, gui_test_device_consumed(&g_gui_test_keyboard));

    /* 一次 step 最多投递 256 项，余项让等待时间为 0。 */
    uint32_t delay = 12345;
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
    assert_eq(256, log.count);
    assert_eq(0, delay);

    gui_input_destroy(input);
    gui_platform_destroy(platform);

    /* 鼠标预算：每轮最多 64 个事件。 */
    gui_test_reset();
    platform = gui_platform_create(NULL);
    assert_not_null(platform);

    input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    for (int i = 0; i < 100; i++) {
        push_mouse(&g_gui_test_mouse, 0, 1, 0, 0);
    }
    gui_test_device_add_op(&g_gui_test_mouse, 0, 100 * 8);
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(64 * 8, gui_test_device_consumed(&g_gui_test_mouse));

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_pump_down_up_not_merged)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    /* 同一轮里到达的按下与释放都必须送达。 */
    uint8_t make_a = 0x1E;
    uint8_t break_a = 0x9E;
    gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
    gui_test_device_add_data(&g_gui_test_keyboard, &break_a, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 2);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    uint32_t delay = 0;
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));

    assert_eq(2, log.count);
    assert_eq(GUI_KEY_DOWN, log.events[0].action);
    assert_eq(GUI_KEY_UP, log.events[1].action);
    assert_eq(log.events[0].physical_key, log.events[1].physical_key);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_pump_queued_snapshot_travels_with_event)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    /* 'a' 先按下，Ctrl 后按下：'a' 的快照是**它入队时**的状态。 */
    uint8_t seq[2] = { 0x1E, 0x1D };   /* make 'a'，make 左 Ctrl */
    gui_test_device_add_data(&g_gui_test_keyboard, seq, sizeof(seq));
    gui_test_device_add_op(&g_gui_test_keyboard, 0, sizeof(seq));

    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    uint32_t delay = 0;
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));

    assert_eq(2, log.count);
    /* 'a' 入队时 Ctrl 还没有按下：快照不得被解码器的当前状态覆盖。 */
    assert_eq(0, log.events[0].modifiers);
    assert_eq('a', log.events[0].codepoint);
    /* Ctrl 的快照带上自己的位。 */
    assert_eq(GUI_MOD_LCTRL, log.events[1].modifiers);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_pump_queued_ctrl_cancels_current_key)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    lv_indev_t *keypad = find_indev(LV_INDEV_TYPE_KEYPAD);
    assert_not_null(keypad);

    uint32_t delay = 0;

    /* 第一步：'a' 按下并进入 keypad。 */
    uint8_t make_a = 0x1E;
    gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
    assert_eq(1, log.count);
    assert_eq('a', lv_indev_get_key(keypad));

    /* 第二步：排队的 Ctrl DOWN 取消当前键（wait-release 重新就绪）。 */
    uint8_t make_ctrl = 0x1D;
    gui_test_device_add_data(&g_gui_test_keyboard, &make_ctrl, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
    assert_eq(2, log.count);
    assert_eq(GUI_MOD_LCTRL, log.events[1].modifiers);
    /* 旧键不再处于按下状态：取消后的合成 RELEASED 已经重新就绪。 */
    assert_eq(0, lv_indev_get_key(keypad));

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_platform_callbacks_only_inside_step)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    key_log_t klog;
    pointer_log_t plog;
    memset(&klog, 0, sizeof(klog));
    memset(&plog, 0, sizeof(plog));
    gui_input_set_key_handler(input, record_key, &klog);
    gui_input_set_pointer_handler(input, record_pointer, &plog);

    uint8_t make_a = 0x1E;
    gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);
    push_mouse(&g_gui_test_mouse, 0x01, 5, 7, 0);
    gui_test_device_add_op(&g_gui_test_mouse, 0, 8);

    /* dispatch 只读入队：不调用任何回调，也不触碰 LVGL。 */
    assert_eq(0, gui_input_dispatch(input, NULL, 0));
    assert_eq(0, klog.count);
    assert_eq(0, plog.count);

    lv_indev_t *keypad = find_indev(LV_INDEV_TYPE_KEYPAD);
    assert_not_null(keypad);
    uint32_t key_before = lv_indev_get_key(keypad);

    /* 回调只在 step 内执行。 */
    uint32_t delay = 0;
    assert_eq(GUI_STATUS_OK, gui_platform_step(platform, &delay));
    assert_eq(1, klog.count);
    assert_eq(1, plog.count);
    assert_eq('a', klog.events[0].codepoint);
    assert_eq(1, plog.events[0].buttons);
    assert_eq(1, plog.events[0].changed_buttons);
    assert_true(lv_indev_get_key(keypad) != key_before);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_platform_lost_owner_stops_delivery)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    key_log_t log;
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(input, record_key, &log);

    uint8_t make_a = 0x1E;
    gui_test_device_add_data(&g_gui_test_keyboard, &make_a, 1);
    gui_test_device_add_op(&g_gui_test_keyboard, 0, 1);
    assert_eq(0, gui_input_dispatch(input, NULL, 0));

    /* 画面静止（没有 flush / present）时失去前台所有权。 */
    g_gui_test_display_flags = 0;

    uint32_t delay = 0;
    errno = 0;
    gui_status_t status = gui_platform_step(platform, &delay);
    assert_eq(GUI_STATUS_LOST_OWNERSHIP, status);
    assert_eq(EACCES, errno);

    /* 交付被终止：没有任何事件到达应用。 */
    assert_eq(0, log.count);

    /* 终止态下输入已经停止：不再提供可 poll 的 fd。 */
    struct pollfd fds[2];
    errno = 0;
    assert_eq(-1, gui_input_get_pollfds(input, fds));
    assert_eq(EACCES, errno);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_platform_input_error_enters_terminal_path)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };
    gui_input_t *input = gui_input_create(platform, &cfg);
    assert_not_null(input);

    uint32_t closes_before = g_gui_test_counts.fd_close;
    uint32_t unbinds_before = g_gui_test_counts.unbind;
    uint32_t releases_before = g_gui_test_counts.release;

    /* 读错误 EACCES → LOST_OWNERSHIP。 */
    gui_test_device_add_op(&g_gui_test_keyboard, EACCES, 0);
    errno = 0;
    assert_eq(-1, gui_input_dispatch(input, NULL, 0));
    assert_eq(EACCES, errno);

    uint32_t delay = 0;
    errno = 0;
    gui_status_t status = gui_platform_step(platform, &delay);
    assert_eq(GUI_STATUS_LOST_OWNERSHIP, status);
    assert_eq(EACCES, errno);

    /* 终止清理：先停输入（解绑 + 关闭 fd），再释放显示。 */
    assert_eq(unbinds_before + 1, g_gui_test_counts.unbind);
    assert_eq(closes_before + 2, g_gui_test_counts.fd_close);
    assert_eq(releases_before + 1, g_gui_test_counts.release);

    gui_input_destroy(input);
    gui_platform_destroy(platform);
}

TEST_FUNC(test_create_destroy_cycles_return_to_baseline)
{
    gui_test_reset();
    gui_platform_t *platform = gui_platform_create(NULL);
    assert_not_null(platform);

    int indevs_before = count_indevs();
    int timers_before = count_timers();
    uint32_t opens_before = g_gui_test_counts.open;
    uint32_t closes_before = g_gui_test_counts.fd_close;
    uint32_t binds_before = g_gui_test_counts.bind;
    uint32_t unbinds_before = g_gui_test_counts.unbind;

    gui_input_config_t cfg = { .mode = GUI_INPUT_DEVICES, .require_mouse = false };

    for (int i = 0; i < 100; i++) {
        gui_input_t *input = gui_input_create(platform, &cfg);
        assert_not_null(input);
        gui_input_destroy(input);
    }

    /* 真实的 LVGL 释放路径：indev / group / timer 全部归还。 */
    assert_eq(indevs_before, count_indevs());
    assert_eq(timers_before, count_timers());

    /* fd 与租约一一对应。 */
    assert_eq(opens_before + 200, g_gui_test_counts.open);
    assert_eq(closes_before + 200, g_gui_test_counts.fd_close);
    assert_eq(binds_before + 100, g_gui_test_counts.bind);
    assert_eq(unbinds_before + 100, g_gui_test_counts.unbind);

    gui_platform_destroy(platform);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_lifecycle_create_and_destroy),
    TEST_ENTRY(test_lifecycle_rollback_each_stage),
    TEST_ENTRY(test_lifecycle_mouse_degradation),
    TEST_ENTRY(test_lifecycle_second_create_is_ebusy),
    TEST_ENTRY(test_lifecycle_synthetic_and_mode_rules),
    TEST_ENTRY(test_lifecycle_failed_create_keeps_previous_owner),
    TEST_ENTRY(test_caps_only_seeding),
    TEST_ENTRY(test_pump_read_errors),
    TEST_ENTRY(test_pump_short_read_carries_prefix),
    TEST_ENTRY(test_pump_poll_flags_map_to_errors),
    TEST_ENTRY(test_pump_budgets_and_fifo_capacity),
    TEST_ENTRY(test_pump_down_up_not_merged),
    TEST_ENTRY(test_pump_queued_snapshot_travels_with_event),
    TEST_ENTRY(test_pump_queued_ctrl_cancels_current_key),
    TEST_ENTRY(test_platform_callbacks_only_inside_step),
    TEST_ENTRY(test_platform_lost_owner_stops_delivery),
    TEST_ENTRY(test_platform_input_error_enters_terminal_path),
    TEST_ENTRY(test_create_destroy_cycles_return_to_baseline),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
