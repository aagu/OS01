/*
 * input.c — 有界输入生命周期、FIFO、公平泵与平台收敛
 *
 * 本文件实现 Task 5 声明的全部公共 API（gui_input_create / destroy /
 * get_pollfds / has_mouse / dispatch / set_*_handler），并把输入对象挂
 * 到平台上：gui_platform_step 在投递 FIFO 之前先检查显示所有权与已锁存
 * 的输入错误，然后才把事件交给 LVGL。
 *
 * 设计要点（spec §5.1 / §5.2）：
 *
 *   - 创建顺序固定：平台存活 → 打开设备 → 分配固定容量状态与 LVGL
 *     对象 → acquire 视图 → bind 租约并读取初始 Caps → 挂接平台；失败
 *     逆序回滚并保留**第一个** errno。
 *   - FIFO 固定 512 项，保存物理/键盘边沿原序；入队时刻的修饰键快照
 *     随事件一起排队，投递时不得用解码器的当前状态覆盖（那会把排队的
 *     修饰键边沿重复计算）。重复策略使用的已投递状态另存。
 *   - 泵是公平的：每轮键盘最多 256 字节、鼠标最多 64 个事件，两类设备
 *     各有预算；每次读取还受 FIFO 剩余容量约束，FIFO 接近满时停止
 *     读取而不是制造假溢出。
 *   - 永久读失败被锁存：EACCES → LOST_OWNERSHIP、ESTALE →
 *     DISPLAY_CHANGED、其余（EOVERFLOW/EIO/EBADF/EBUSY）→ ERROR。
 *     内核的 DESYNC 状态（跨 epoch 的序列被吞掉）与队列溢出一样报
 *     EOVERFLOW，因此它到达应用时是 ERROR，而不是 DISPLAY_CHANGED：
 *     那个租约已经不可信，应用必须交还租约并重新绑定。
 */

#include <gui/gui_input.h>
#include <gui_input_private.h>
#include <gui_platform_private.h>
#include <gui_tick.h>
#include <gfx.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GUI_INPUT_KEYBOARD_PATH "/dev/keyboard"
#define GUI_INPUT_MOUSE_PATH    "/dev/mouse"

/* ── 错误 → 平台状态 ──────────────────────────────────────────── */

static gui_status_t status_for_errno(int err)
{
    switch (err) {
    case EACCES:
        return GUI_STATUS_LOST_OWNERSHIP;
    case ESTALE:
        return GUI_STATUS_DISPLAY_CHANGED;
    default:
        return GUI_STATUS_ERROR;
    }
}

/* 锁存第一个永久错误；后续错误不覆盖它。 */
static void latch_error(gui_input_t *input, int err)
{
    if (err == 0) {
        err = EIO;
    }
    if (input->latched_errno == 0) {
        input->latched_errno = err;
        input->latched_status = status_for_errno(err);
    }
    errno = input->latched_errno;
}

/* ── FIFO（固定容量 512 项） ──────────────────────────────────── */

static unsigned fifo_room(const gui_input_t *input)
{
    return GUI_INPUT_FIFO_CAPACITY - input->count;
}

static bool fifo_push(gui_input_t *input, const gui_input_item_t *item)
{
    if (input->count >= GUI_INPUT_FIFO_CAPACITY) {
        return false;
    }
    unsigned tail = (input->head + input->count) % GUI_INPUT_FIFO_CAPACITY;
    input->items[tail] = *item;
    input->count++;
    return true;
}

static bool fifo_pop(gui_input_t *input, gui_input_item_t *out)
{
    if (input->count == 0) {
        return false;
    }
    *out = input->items[input->head];
    input->head = (input->head + 1) % GUI_INPUT_FIFO_CAPACITY;
    input->count--;
    return true;
}

static void fifo_clear(gui_input_t *input)
{
    input->head = 0;
    input->count = 0;
}

/* ── 设备读取 ─────────────────────────────────────────────────── */

/*
 * 把一个已解码的按键事件排入 FIFO。
 *
 * 设备路径与合成的测试注入共用这一条路径：事件带着**入队时刻**的修饰键
 * 快照排队，step 投递时不得用解码器的当前状态覆盖它。
 */
static int key_event_push(gui_input_t *input, const gui_key_event_t *ev, uint64_t now_ms)
{
    gui_input_item_t item;

    memset(&item, 0, sizeof(item));
    item.kind = GUI_INPUT_ITEM_KEY;
    item.action = (uint8_t)ev->action;
    item.physical_key = ev->physical_key;
    item.modifiers = ev->modifiers;  /* 快照随事件入队 */
    item.codepoint = ev->codepoint;
    item.time_ms = now_ms;

    if (!fifo_push(input, &item)) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

/*
 * 读取一次键盘数据并解码入队。
 *
 * 返回 1 = 读到并处理了数据；0 = 本轮没有更多数据（EAGAIN / 空读）；
 * -1 = 永久错误（errno 已设置，尚未锁存）。
 */
static int keyboard_read_once(gui_input_t *input, uint64_t now_ms, unsigned budget,
                              unsigned *budget_left)
{
    unsigned room = fifo_room(input);
    if (room == 0) {
        return 0;   /* FIFO 已满：停止读取，交给下一次 step 消费 */
    }

    size_t want = budget;
    if (want > room) {
        want = room;   /* 读取量受 FIFO 剩余容量约束 */
    }
    if (want == 0) {
        return 0;
    }

    uint8_t buf[GUI_INPUT_KEYBOARD_READ_BUDGET];
    ssize_t n = read(input->keyboard_fd, buf, want);
    if (n < 0) {
        int err = errno;
        if (err == EINTR) {
            return 0;   /* 中断：下一轮继续 */
        }
        if (err == EAGAIN) {
            return 0;   /* 本轮结束 */
        }
        errno = err;
        return -1;
    }
    if (n == 0) {
        /* 旧 NORMAL 路径的空读不是永久 EOF：只是本轮没有数据。 */
        return 0;
    }

    *budget_left = budget - (unsigned)n;

    for (ssize_t i = 0; i < n; i++) {
        gui_key_event_t ev;
        int rc = gui_ps2_feed(&input->ps2, buf[i], now_ms, &ev);
        if (rc < 0) {
            errno = EINVAL;
            return -1;
        }
        if (rc == 0) {
            continue;   /* 前缀 / 被吞掉的序列 */
        }

        if (key_event_push(input, &ev, now_ms) != 0) {
            return -1;
        }
    }
    return 1;
}

/*
 * 读取一次鼠标数据并入队。
 *
 * 鼠标记录固定 8 字节：长度不是 8 的倍数视为损坏（EIO）。
 */
static int mouse_read_once(gui_input_t *input, uint64_t now_ms, unsigned budget,
                           unsigned *budget_left)
{
    unsigned room = fifo_room(input);
    if (room == 0) {
        return 0;
    }

    size_t want = (size_t)budget * sizeof(mouse_event_t);
    size_t room_bytes = (size_t)room * sizeof(mouse_event_t);
    if (want > room_bytes) {
        want = room_bytes;
    }
    if (want == 0) {
        return 0;
    }

    uint8_t buf[GUI_INPUT_MOUSE_READ_BUDGET * sizeof(mouse_event_t)];
    ssize_t n = read(input->mouse_fd, buf, want);
    if (n < 0) {
        int err = errno;
        if (err == EINTR) {
            return 0;
        }
        if (err == EAGAIN) {
            return 0;
        }
        errno = err;
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if ((size_t)n % sizeof(mouse_event_t) != 0) {
        errno = EIO;   /* 不完整的记录 */
        return -1;
    }

    *budget_left = budget - (unsigned)((size_t)n / sizeof(mouse_event_t));

    for (ssize_t off = 0; off < n; off += (ssize_t)sizeof(mouse_event_t)) {
        mouse_event_t raw;
        memcpy(&raw, buf + off, sizeof(raw));

        gui_input_item_t item;
        memset(&item, 0, sizeof(item));
        item.kind = GUI_INPUT_ITEM_POINTER;
        item.time_ms = now_ms;
        item.raw = raw;
        item.changed_buttons = (uint8_t)(raw.buttons ^ input->pointer_buttons);
        input->pointer_buttons = raw.buttons;

        if (!fifo_push(input, &item)) {
            errno = EOVERFLOW;
            return -1;
        }
    }
    return 1;
}

/*
 * 单个设备的泵：处理 poll 标志并在本轮预算内读取。
 *
 * poll_err  → 做一次非阻塞读以取得内核锁存的原因；
 * poll_hup  → 设备消失（ESTALE）；
 * POLLNVAL  → 直接按 EBADF 处理（在调用方先行判定）。
 */
static int pump_device(gui_input_t *input, bool poll_err, bool poll_hup,
                       bool is_mouse, uint64_t now_ms)
{
    unsigned budget = is_mouse ? GUI_INPUT_MOUSE_READ_BUDGET
                               : GUI_INPUT_KEYBOARD_READ_BUDGET;

    if (poll_err) {
        /* 一次非阻塞读：内核把真正的原因锁存在读错误里。 */
        unsigned left = budget;
        int rc = is_mouse ? mouse_read_once(input, now_ms, budget, &left)
                          : keyboard_read_once(input, now_ms, budget, &left);
        if (rc < 0) {
            return -1;
        }
        budget = left;
    }

    if (poll_hup) {
        errno = ESTALE;
        return -1;
    }

    while (budget > 0) {
        unsigned left = budget;
        int rc = is_mouse ? mouse_read_once(input, now_ms, budget, &left)
                          : keyboard_read_once(input, now_ms, budget, &left);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            break;   /* 没有更多数据 */
        }
        budget = left;
    }
    return 0;
}

/* ── 公共 API：创建与销毁 ─────────────────────────────────────── */

gui_input_t *gui_input_create(gui_platform_t *platform, const gui_input_config_t *cfg)
{
    if (!platform) {
        errno = EINVAL;
        return NULL;
    }

    gui_input_mode_t mode = GUI_INPUT_DEVICES;
    bool require_mouse = false;
    if (cfg) {
        mode = cfg->mode;
        require_mouse = cfg->require_mouse;
    }

    /* 1. 平台必须存活，且尚未挂接输入（每平台至多一个）。 */
    if (!gui_platform_alive(platform)) {
        errno = EINVAL;
        return NULL;
    }
    if (gui_platform_input_attached(platform)) {
        errno = EBUSY;
        return NULL;
    }

    if (mode != GUI_INPUT_DEVICES && mode != GUI_INPUT_SYNTHETIC) {
        errno = EINVAL;
        return NULL;
    }

    gui_mode_t pmode = gui_platform_get_mode(platform);
    if (mode == GUI_INPUT_DEVICES && pmode != GUI_MODE_ONSCREEN) {
        errno = EINVAL;   /* 真实设备只允许屏幕平台 */
        return NULL;
    }
    if (mode == GUI_INPUT_SYNTHETIC && pmode != GUI_MODE_OFFSCREEN) {
        errno = EINVAL;   /* 合成输入只允许离屏平台 */
        return NULL;
    }

    int saved = 0;
    int keyboard_fd = -1;
    int mouse_fd = -1;
    gui_input_t *input = NULL;

    /* 2. 打开设备（O_RDONLY|O_NONBLOCK）。 */
    if (mode == GUI_INPUT_DEVICES) {
        keyboard_fd = open(GUI_INPUT_KEYBOARD_PATH, O_RDONLY | O_NONBLOCK);
        if (keyboard_fd < 0) {
            return NULL;   /* 键盘必需：errno 原样透传 */
        }

        mouse_fd = open(GUI_INPUT_MOUSE_PATH, O_RDONLY | O_NONBLOCK);
        if (mouse_fd < 0) {
            int err = errno;
            /* 鼠标只允许因 ENOENT/ENODEV 降级为 keyboard-only。 */
            if (require_mouse || (err != ENOENT && err != ENODEV)) {
                saved = err;
                goto rollback;
            }
            mouse_fd = -1;
        }
    }

    /* 3. 分配固定容量状态与 LVGL 对象。 */
    input = (gui_input_t *)calloc(1, sizeof(*input));
    if (!input) {
        saved = ENOMEM;
        goto rollback;
    }
    input->platform = platform;
    input->mode = mode;
    input->require_mouse = require_mouse;
    input->keyboard_fd = keyboard_fd;
    input->mouse_fd = mouse_fd;
    input->latched_status = GUI_STATUS_OK;
    keyboard_fd = -1;
    mouse_fd = -1;

    if (gui_input_lvgl_create(input) != 0) {
        saved = errno ? errno : ENOMEM;
        goto rollback;
    }

    if (mode == GUI_INPUT_DEVICES) {
        /* 4. 取得显示所有权（租约只能由 owner 绑定）。 */
        bool acquired_here = false;
        if (gui_platform_acquire_view(platform, &acquired_here) != 0) {
            saved = errno ? errno : EIO;
            goto rollback;
        }
        input->acquired_here = acquired_here;
        input->gfx = gui_platform_gfx_handle(platform);
        if (!input->gfx) {
            saved = EINVAL;
            goto rollback;
        }

        /* 5. 绑定输入租约并读取初始 Caps 锁存。 */
        gfx_input_state_t state;
        memset(&state, 0, sizeof(state));
        errno = 0;
        if (gfx_bind_input(input->gfx, input->keyboard_fd,
                           input->mouse_fd >= 0 ? input->mouse_fd : -1,
                           &state) != 0) {
            saved = errno ? errno : EIO;
            goto rollback;
        }
        input->bound = true;

        /*
         * 只植入 Caps 锁存位。
         *
         * 位序转换：租约 UAPI 的快照是 **bit0 = Caps Lock**
         * （uapi/gfx.h；内核 fb_state 只把 INPUT_ROUTE_MOD_CAPS 折算成
         * 1u 交给用户态），而解码器用的是公开 gui_modifier_t 位序
         * （CAPS = 1 << 6）。因此这里先把 UAPI 的 bit0 折算成
         * GUI_MOD_CAPS，再用 GUI_MOD_CAPS 掩码，保证**只有** Caps 进入
         * 解码器。
         *
         * 为什么不能转发聚合位：bind 时内核把当时仍按住的**普通键**标记
         * 为隔离并吞掉它们的释放沿，快照里的聚合 Shift/Ctrl/Alt 位描述的
         * 是消费者**从未看到**的按下边沿——据此认为修饰键按住了，就是相信
         * 了一个它没有观察到的状态。而且内核根本不把这些位交给用户态：
         * 租约 UAPI 只发布 Caps 位。Caps 锁存位不同：它是**状态**而不是
         * 边沿，消费者没有别的途径知道它，所以它必须被植入。
         */
        uint16_t seed = 0;
        if (state.initial_modifiers & GUI_INPUT_LEASE_MOD_CAPS) {
            seed |= (uint16_t)GUI_MOD_CAPS;
        }
        seed = (uint16_t)(seed & (uint16_t)GUI_MOD_CAPS);
        gui_ps2_init(&input->ps2, seed);
    } else {
        gui_ps2_init(&input->ps2, 0);
    }

    /* 6. 挂接平台。 */
    gui_platform_attach_input(platform, input);
    return input;

rollback:
    if (input) {
        if (input->bound && input->gfx) {
            gfx_unbind_input(input->gfx);
            input->bound = false;
        }
        if (input->acquired_here && input->gfx) {
            gfx_release(input->gfx);
            input->acquired_here = false;
        }
        gui_input_lvgl_destroy(input);
        if (input->mouse_fd >= 0) {
            close(input->mouse_fd);
            input->mouse_fd = -1;
        }
        if (input->keyboard_fd >= 0) {
            close(input->keyboard_fd);
            input->keyboard_fd = -1;
        }
        free(input);
    } else {
        if (mouse_fd >= 0) {
            close(mouse_fd);
        }
        if (keyboard_fd >= 0) {
            close(keyboard_fd);
        }
    }
    errno = saved;
    return NULL;
}

void gui_input_stop(gui_input_t *input, int reason)
{
    if (!input) {
        return;
    }
    if (input->stopped) {
        return;   /* 幂等 */
    }
    input->stopped = true;

    if (reason != 0 && input->latched_errno == 0) {
        input->latched_errno = reason;
        input->latched_status = status_for_errno(reason);
    }

    /* 回调先停：停止之后不再有事件进入应用。 */
    input->key_handler = NULL;
    input->pointer_handler = NULL;
    input->key_userdata = NULL;
    input->pointer_userdata = NULL;

    /* 未投递的事件被丢弃。 */
    fifo_clear(input);

    /* 先解绑租约，再关闭原始设备文件。 */
    if (input->bound) {
        if (input->gfx) {
            gfx_unbind_input(input->gfx);
        }
        input->bound = false;
    }
    if (input->mouse_fd >= 0) {
        close(input->mouse_fd);
        input->mouse_fd = -1;
    }
    if (input->keyboard_fd >= 0) {
        close(input->keyboard_fd);
        input->keyboard_fd = -1;
    }
}

void gui_input_destroy(gui_input_t *input)
{
    if (!input) {
        return;
    }

    gui_input_stop(input, input->latched_errno);

    /* 删除 indev / group（destroy 不释放平台仍拥有的 gfx）。 */
    gui_input_lvgl_destroy(input);

    if (input->platform) {
        gui_platform_detach_input(input->platform, input);
        input->platform = NULL;
    }

    free(input);
}

/* ── 公共 API：查询 ───────────────────────────────────────────── */

int gui_input_get_pollfds(gui_input_t *input, struct pollfd out[2])
{
    if (!input || !out) {
        errno = EINVAL;
        return -1;
    }
    if (input->latched_errno) {
        errno = input->latched_errno;
        return -1;
    }
    if (input->stopped) {
        errno = EBADF;
        return -1;
    }
    if (input->mode == GUI_INPUT_SYNTHETIC) {
        return 0;   /* 合成输入没有设备 */
    }

    int count = 0;
    out[count].fd = input->keyboard_fd;
    out[count].events = POLLIN;
    out[count].revents = 0;
    count++;

    if (input->mouse_fd >= 0) {
        out[count].fd = input->mouse_fd;
        out[count].events = POLLIN;
        out[count].revents = 0;
        count++;
    }
    return count;
}

bool gui_input_has_mouse(const gui_input_t *input)
{
    if (!input) {
        return false;
    }
    return input->mouse_fd >= 0;
}

void gui_input_set_key_handler(gui_input_t *input, gui_key_handler_t handler,
                               void *userdata)
{
    if (!input) {
        errno = EINVAL;
        return;
    }
    input->key_handler = handler;
    input->key_userdata = handler ? userdata : NULL;
}

void gui_input_set_pointer_handler(gui_input_t *input, gui_pointer_handler_t handler,
                                   void *userdata)
{
    if (!input) {
        errno = EINVAL;
        return;
    }
    input->pointer_handler = handler;
    input->pointer_userdata = handler ? userdata : NULL;
}

/* ── 公共 API：泵 ─────────────────────────────────────────────── */

int gui_input_dispatch(gui_input_t *input, const struct pollfd *fds, unsigned count)
{
    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (input->latched_errno) {
        errno = input->latched_errno;
        return -1;
    }
    if (input->stopped) {
        errno = EBADF;
        return -1;
    }
    if (input->mode == GUI_INPUT_SYNTHETIC) {
        return 0;   /* 合成输入不接触设备 */
    }

    /* 校验 fd/count：(NULL, 0) 合法，表示立即检查本对象的设备。 */
    if (!fds && count != 0) {
        errno = EINVAL;
        return -1;
    }
    if (count > 2) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd kfd;
    struct pollfd mfd;
    bool have_kfd = false;
    bool have_mfd = false;
    memset(&kfd, 0, sizeof(kfd));
    memset(&mfd, 0, sizeof(mfd));

    if (fds && count > 0) {
        for (unsigned i = 0; i < count; i++) {
            if (fds[i].fd == input->keyboard_fd) {
                kfd = fds[i];
                have_kfd = true;
            } else if (input->mouse_fd >= 0 && fds[i].fd == input->mouse_fd) {
                mfd = fds[i];
                have_mfd = true;
            } else {
                errno = EINVAL;   /* 不是本对象的设备 */
                return -1;
            }
        }
    }

    /* POLLNVAL 直接按 EBADF 处理：不能只凭 poll 标志丢失区分度。 */
    if ((have_kfd && (kfd.revents & POLLNVAL)) ||
        (have_mfd && (mfd.revents & POLLNVAL))) {
        latch_error(input, EBADF);
        return -1;
    }

    uint64_t now_ms = 0;
    if (gui_tick_now_ms(&now_ms) != 0) {
        latch_error(input, errno ? errno : EIO);
        return -1;
    }

    /* 每轮固定 keyboard 后 mouse：跨设备没有硬件时间戳。 */
    if (pump_device(input, have_kfd && (kfd.revents & POLLERR),
                    have_kfd && (kfd.revents & POLLHUP),
                    false, now_ms) != 0) {
        latch_error(input, errno);
        return -1;
    }

    if (input->mouse_fd >= 0) {
        if (pump_device(input, have_mfd && (mfd.revents & POLLERR),
                        have_mfd && (mfd.revents & POLLHUP),
                        true, now_ms) != 0) {
            latch_error(input, errno);
            return -1;
        }
    }

    return 0;
}

/* ── 投递（只在 step 内发生） ─────────────────────────────────── */

/*
 * 投递一个键事件。
 *
 * 顺序：更新已投递状态 → Ctrl/Alt 从无到有时取消当前键 → 新的非修饰键
 * DOWN 取消旧活动键 → 武装 / 解除软件重复 → 报告原生 handler →
 * 未被消费时送入 keypad。
 *
 * 取消动作排在报告原生 handler **之前**：modifier DOWN 到达应用时，旧键
 * 必须已经被取消（spec §6.2.1）。
 */
static void deliver_key_event(gui_input_t *input, const gui_key_event_t *ev,
                              uint64_t now_ms)
{
    /* 已投递状态与排队快照分开保存（重复策略使用前者）。 */
    uint16_t prev = input->delivered_modifiers;
    uint16_t now = ev->modifiers;

    /* Ctrl/Alt 从无到有：取消当前键与它的重复，排队的 Ctrl 不得泄漏到
     * 后续按键。 */
    const uint16_t ctrl_alt = (uint16_t)(GUI_MOD_LCTRL | GUI_MOD_RCTRL |
                                         GUI_MOD_LALT | GUI_MOD_RALT);
    if ((now & ctrl_alt) && !(prev & ctrl_alt)) {
        gui_input_lvgl_cancel(input);
    }
    input->delivered_modifiers = now;

    if (ev->action == GUI_KEY_DOWN) {
        /*
         * 新的按键按下：先取消旧活动键。新键即使随后被应用消费，旧键也
         * 已经失效（LVGL 的 keypad 是单键模型）。
         *
         * 软件重复**不在这里**武装：只有真正到达 LVGL 的按键才允许重复。
         */
        gui_input_lvgl_key_down(input, ev);
    }

    bool consumed = false;
    if (input->key_handler) {
        consumed = input->key_handler(ev, input->key_userdata);
    }

    if (!consumed) {
        int delivered = gui_input_lvgl_deliver(input, ev);
        if (ev->action == GUI_KEY_DOWN) {
            /*
             * 只有**真正进入 LVGL** 的按键才武装软件重复：应用消费掉的
             * 按键从未进入 LVGL，不得为它产生 REPEAT（应用会执行一个自己
             * 并没有交给控件的动作）。
             */
            gui_input_lvgl_repeat_after_down(input, ev, delivered != 0, now_ms);
        }
    } else if (ev->action == GUI_KEY_DOWN) {
        /* 被消费的按下从未进入 LVGL：不得为它产生 REPEAT。 */
        gui_input_lvgl_repeat_after_down(input, ev, false, now_ms);
    } else if (ev->action == GUI_KEY_UP) {
        /*
         * 被消费的释放仍然必须释放 LVGL 侧的活动键，否则 LVGL 会一直认为
         * 那个键处于按下状态。
         */
        gui_input_lvgl_force_release(input, ev->physical_key);
    }
}

static void deliver_key(gui_input_t *input, const gui_input_item_t *item)
{
    gui_key_event_t ev;
    ev.physical_key = item->physical_key;
    ev.modifiers = item->modifiers;   /* 排队时的快照，绝不被覆盖 */
    ev.codepoint = item->codepoint;
    ev.action = (gui_key_action_t)item->action;

    deliver_key_event(input, &ev, item->time_ms);
}

static void deliver_pointer(gui_input_t *input, const gui_input_item_t *item)
{
    gui_input_lvgl_pointer(input, &item->raw);

    if (input->pointer_handler) {
        gui_pointer_event_t ev;
        ev.x = input->lvgl.cursor_x;
        ev.y = input->lvgl.cursor_y;
        ev.buttons = item->raw.buttons;
        ev.changed_buttons = item->changed_buttons;
        ev.wheel = item->raw.wheel;
        input->pointer_handler(&ev, input->pointer_userdata);
    }
}

/* ── 平台推进 ─────────────────────────────────────────────────── */

/* 显示所有权检查：即使没有脏帧也不能继续自动输入。 */
static gui_status_t check_owner(gui_input_t *input)
{
    if (!input->gfx) {
        return GUI_STATUS_OK;
    }

    gfx_display_state_t state;
    memset(&state, 0, sizeof(state));
    errno = 0;
    if (gfx_get_display_state(input->gfx, &state) != 0) {
        int err = errno ? errno : EIO;
        errno = err;
        return status_for_errno(err);
    }
    if ((state.flags & GFX_DISPLAY_STATE_FOREGROUND) == 0) {
        errno = EACCES;
        return GUI_STATUS_LOST_OWNERSHIP;
    }
    return GUI_STATUS_OK;
}

gui_status_t gui_input_advance(gui_input_t *input, uint64_t now_ms, uint32_t *delay_ms)
{
    if (delay_ms) {
        *delay_ms = UINT32_MAX;   /* UINT32_MAX = 不约束等待时间 */
    }
    if (!input) {
        errno = EINVAL;
        return GUI_STATUS_ERROR;
    }

    /* 已锁存的输入错误优先：死掉的输入不再投递任何事件。 */
    if (input->latched_errno) {
        errno = input->latched_errno;
        return input->latched_status;
    }
    if (input->stopped) {
        errno = EBADF;
        return GUI_STATUS_ERROR;
    }

    /* 所有权检查在投递之前。 */
    if (input->mode == GUI_INPUT_DEVICES) {
        gui_status_t st = check_owner(input);
        if (st != GUI_STATUS_OK) {
            return st;
        }
    }

    /* 投递 FIFO：每步至多 256 项。 */
    unsigned delivered = 0;
    gui_input_item_t item;
    while (delivered < GUI_INPUT_DELIVERY_BUDGET && fifo_pop(input, &item)) {
        if (item.kind == GUI_INPUT_ITEM_KEY) {
            deliver_key(input, &item);
        } else {
            deliver_pointer(input, &item);
        }
        delivered++;
    }

    /*
     * 适配层维护：焦点检查与待处理的取消协议（重新就绪）。
     * 取消尚未完成时不投递新键。
     */
    bool keypad_ready = gui_input_lvgl_pump(input);

    /*
     * 软件重复：首次 400 ms、之后 40 ms。
     *
     * 只在 FIFO 排空后生成：FIFO 里还有未投递的 UP / Ctrl 时，重复不得
     * 插到它们前面。每次最多产生一个到期重复（停顿期间不补发）。
     */
    if (keypad_ready && input->count == 0 && input->repeat_armed &&
        now_ms >= input->repeat_deadline_ms) {
        gui_key_event_t rep;
        rep.action = GUI_KEY_REPEAT;
        rep.physical_key = input->repeat_physical;
        rep.codepoint = input->repeat_char;   /* DOWN 时缓存的字符 */
        rep.modifiers = input->delivered_modifiers;  /* 当前已投递快照 */
        /* 下一期限从当前时刻向后计算：停顿期间不补发一串重复。 */
        input->repeat_deadline_ms = now_ms + GUI_INPUT_REPEAT_INTERVAL_MS;
        deliver_key_event(input, &rep, now_ms);
    }

    /* 截断的前缀到期后被丢弃。 */
    gui_ps2_expire(&input->ps2, now_ms);

    if (delay_ms) {
        if (input->count > 0) {
            *delay_ms = 0;   /* 还有余项：立即继续 */
        } else if (input->ps2.prefix != 0) {
            uint64_t elapsed = now_ms >= input->ps2.prefix_ms
                                   ? (now_ms - input->ps2.prefix_ms) : 0;
            uint64_t left = (elapsed >= GUI_PS2_PREFIX_TIMEOUT_MS)
                                ? 0 : (GUI_PS2_PREFIX_TIMEOUT_MS - elapsed);
            *delay_ms = (uint32_t)left;
        }
    }

    return GUI_STATUS_OK;
}

#ifdef OS01_HOST_TEST
/*
 * 私有测试注入：把合成的 Set 1 字节喂进**同一条**解码器 / FIFO 路径。
 *
 * 这个接口只在宿主测试构建（-DOS01_HOST_TEST）里存在：目标构建不编译它，
 * 因此它永远不会进入公共 sysroot，也不出现在任何公共头文件里。
 * 只接受合成输入（GUI_INPUT_SYNTHETIC）。
 */
int gui_input_test_feed_keybytes(gui_input_t *input, const uint8_t *bytes, size_t len)
{
    uint64_t now_ms;
    size_t i;

    if (!input || (!bytes && len > 0)) {
        errno = EINVAL;
        return -1;
    }
    if (input->mode != GUI_INPUT_SYNTHETIC) {
        errno = EINVAL;
        return -1;
    }
    if (gui_tick_now_ms(&now_ms) != 0) {
        return -1;
    }

    for (i = 0; i < len; i++) {
        gui_key_event_t ev;
        int rc = gui_ps2_feed(&input->ps2, bytes[i], now_ms, &ev);

        if (rc < 0) {
            errno = EINVAL;
            return -1;
        }
        if (rc == 0) {
            continue;   /* 前缀 / 被吞掉的序列 */
        }
        if (key_event_push(input, &ev, now_ms) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * 私有测试注入：把合成的鼠标事件喂进**同一条** FIFO / 适配层路径。
 *
 * 规则与键盘桥接相同：只在宿主测试构建（-DOS01_HOST_TEST）里存在，目标
 * 构建不编译它，因此它永远不会进入公共 sysroot，也不出现在任何公共头
 * 文件里。只接受合成输入（GUI_INPUT_SYNTHETIC）。
 *
 * 每次调用 = 一次 dispatch，因此**同样受每轮 64 个鼠标事件的预算约束**
 * （GUI_INPUT_MOUSE_READ_BUDGET，与产品路径 gui_input_read_mouse() 一致）：
 * 夹具不能注入产品路径根本送不出来的批量。需要更多事件时多次调用。
 */
int gui_input_test_feed_mouse(gui_input_t *input, const mouse_event_t *events,
                              size_t count)
{
    uint64_t now_ms;
    size_t i;

    if (!input || (!events && count > 0)) {
        errno = EINVAL;
        return -1;
    }
    if (input->mode != GUI_INPUT_SYNTHETIC) {
        errno = EINVAL;
        return -1;
    }
    if (count > GUI_INPUT_MOUSE_READ_BUDGET) {
        errno = EINVAL;
        return -1;
    }
    if (gui_tick_now_ms(&now_ms) != 0) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        gui_input_item_t item;

        memset(&item, 0, sizeof(item));
        item.kind = GUI_INPUT_ITEM_POINTER;
        item.time_ms = now_ms;
        item.raw = events[i];
        /* 与真实设备读到的路径一致：按钮变化是相对上一次入队状态算出的。 */
        item.changed_buttons = (uint8_t)(item.raw.buttons ^ input->pointer_buttons);
        input->pointer_buttons = item.raw.buttons;

        if (!fifo_push(input, &item)) {
            errno = EOVERFLOW;
            return -1;
        }
    }
    return 0;
}
#endif /* OS01_HOST_TEST */
