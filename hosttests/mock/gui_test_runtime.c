#include "gui_test_runtime.h"
#include <src/display/lv_display_private.h>
#include <gui_tick.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct gui_test_counts g_gui_test_counts = {0};

static uint64_t g_gui_test_now_ms = 100;

bool g_gui_test_intercept_calloc = true;
bool g_gui_test_fail_calloc = false;

bool g_gui_test_intercept_mmap = true;
int  g_gui_test_fail_mmap_n = 0;
bool g_gui_test_mock_large_mmap = true;
bool g_gui_test_fail_munmap = false;

bool g_gui_test_intercept_fb = true;
bool g_gui_test_fail_fb_open = false;
int  g_gui_test_fail_fb_open_errno = 0;
bool g_gui_test_fail_fb_read = false;
int  g_gui_test_fail_fb_read_errno = 0;
bool g_gui_test_short_fb_read = false;
struct fb_info g_gui_test_fb_info = {
    .width = 640,
    .height = 480,
    .stride = 2560,
    .bpp = 32,
    .format = FB_FORMAT_RGB32
};

bool g_gui_test_intercept_clock = true;
bool g_gui_test_fail_clock = false;
int  g_gui_test_fail_clock_errno = 0;
int  g_gui_test_fail_clock_after_n = 0;

bool g_gui_test_fail_gfx_open = false;
int  g_gui_test_fail_gfx_open_errno = 0;
bool g_gui_test_fail_gfx_info = false;
bool g_gui_test_mismatch_gfx_info = false;
gfx_info_t g_gui_test_gfx_info_override = {0};

int  g_gui_test_fail_acquire_errno = 0;
int  g_gui_test_fail_present_errno = 0;
uint32_t g_gui_test_present_blocking_ms = 0;
int  g_gui_test_fail_release_errno = 0;
bool g_gui_test_cleanup_change_errno = false;

/* ── GUI-2 Task 6: the raw input devices and the input lease ──── */

bool g_gui_test_kbd_open_fails = false;
int  g_gui_test_kbd_open_errno = 0;
bool g_gui_test_mouse_open_fails = false;
int  g_gui_test_mouse_open_errno = 0;
int  g_gui_test_kbd_open_flags = 0;
int  g_gui_test_mouse_open_flags = 0;

int  g_gui_test_fail_bind_errno = 0;
uint32_t g_gui_test_bind_initial_modifiers = 0;
uint64_t g_gui_test_bind_generation = 1;

int  g_gui_test_fail_display_state_errno = 0;
uint32_t g_gui_test_display_flags = GFX_DISPLAY_STATE_FOREGROUND;
uint64_t g_gui_test_display_generation = 1;

int  g_gui_test_fail_lvgl_create_errno = 0;

gui_test_device_t g_gui_test_keyboard;
gui_test_device_t g_gui_test_mouse;

void gui_test_device_reset(gui_test_device_t *dev)
{
    if (!dev) return;
    memset(dev, 0, sizeof(*dev));
}

void gui_test_device_add_op(gui_test_device_t *dev, int err, size_t len)
{
    if (!dev || dev->op_count >= GUI_TEST_DEVICE_MAX_OPS) return;
    dev->ops[dev->op_count].err = err;
    dev->ops[dev->op_count].len = len;
    dev->op_count++;
}

void gui_test_device_add_data(gui_test_device_t *dev, const void *bytes, size_t len)
{
    if (!dev || !bytes || len == 0) return;
    size_t room = GUI_TEST_DEVICE_MAX_DATA - dev->data_len;
    if (len > room) len = room;
    memcpy(dev->data + dev->data_len, bytes, len);
    dev->data_len += len;
}

size_t gui_test_device_consumed(const gui_test_device_t *dev)
{
    if (!dev) return 0;
    return dev->data_pos;
}

/* The scripted read: the ops describe what each read() returns. */
static ssize_t gui_test_device_read(gui_test_device_t *dev, void *buf, size_t count)
{
    if (dev->op_index < dev->op_count) {
        gui_test_read_op_t *op = &dev->ops[dev->op_index];
        if (op->err != 0) {
            dev->op_index++;
            errno = op->err;
            return -1;
        }
        size_t avail = dev->data_len - dev->data_pos;
        size_t n = op->len < avail ? op->len : avail;
        if (n > count) n = count;
        memcpy(buf, dev->data + dev->data_pos, n);
        dev->data_pos += n;
        if (n >= op->len || dev->data_pos >= dev->data_len) dev->op_index++;
        return (ssize_t)n;
    }
    /* No scripted operation left: the device has nothing more to give. */
    errno = EAGAIN;
    return -1;
}

/* GUI-2b Task 3: the native widget fault bridges.  They are defined here
 * (and not in the widget fixture) because every GUI host binary links the
 * widget unit and the unit must find these symbols in all of them. */
bool g_gui_test_fail_widget_registry = false;
bool g_gui_test_fail_widget_observer = false;

/* GUI-2b Task 7: the focus engine's delete-observer fault bridge.  The
 * focus unit is linked by every GUI host binary, so the symbol lives here
 * for the same reason as the widget ones above. */
bool g_gui_test_fail_focus_observer = false;

/* GUI-2b Task 7 fix round 1: the focus group's rebuild counter. */
uint32_t g_gui_test_focus_rebuild_count = 0;

/* GUI-2b Task 11: the per-step fault bridges for the file navigation
 * commit.  A positive countdown fails that step (the Nth call fails). */
int g_gui_test_fail_widget_create_countdown = 0;
int g_gui_test_fail_focus_register_countdown = 0;

#ifndef GUI_INTEGRATION_TEST
bool g_gui_test_fail_lv_display_create = false;
uint32_t g_gui_test_handler_delay = 20;

lv_display_t *g_gui_test_last_display = NULL;

struct queued_flush {
    lv_area_t area;
    const uint32_t *pixels;
};
#define MAX_QUEUED_FLUSHES 32
static struct queued_flush s_flush_queue[MAX_QUEUED_FLUSHES];
static int s_flush_count = 0;
#endif

void gui_test_reset(void)
{
    memset(&g_gui_test_counts, 0, sizeof(g_gui_test_counts));
    g_gui_test_now_ms = 100;
    gui_tick_reset();

    g_gui_test_intercept_calloc = true;
    g_gui_test_fail_calloc = false;

    g_gui_test_intercept_mmap = true;
    g_gui_test_fail_mmap_n = 0;
    g_gui_test_mock_large_mmap = true;
    g_gui_test_fail_munmap = false;

    g_gui_test_intercept_fb = true;
    g_gui_test_fail_fb_open = false;
    g_gui_test_fail_fb_open_errno = 0;
    g_gui_test_fail_fb_read = false;
    g_gui_test_fail_fb_read_errno = 0;
    g_gui_test_short_fb_read = false;
    g_gui_test_fb_info.width = 640;
    g_gui_test_fb_info.height = 480;
    g_gui_test_fb_info.stride = 2560;
    g_gui_test_fb_info.bpp = 32;
    g_gui_test_fb_info.format = FB_FORMAT_RGB32;

    g_gui_test_intercept_clock = true;
    g_gui_test_fail_clock = false;
    g_gui_test_fail_clock_errno = 0;
    g_gui_test_fail_clock_after_n = 0;

    g_gui_test_fail_gfx_open = false;
    g_gui_test_fail_gfx_open_errno = 0;
    g_gui_test_fail_gfx_info = false;
    g_gui_test_mismatch_gfx_info = false;
    memset(&g_gui_test_gfx_info_override, 0, sizeof(g_gui_test_gfx_info_override));

    g_gui_test_fail_acquire_errno = 0;
    g_gui_test_fail_present_errno = 0;
    g_gui_test_present_blocking_ms = 0;
    g_gui_test_fail_release_errno = 0;
    g_gui_test_cleanup_change_errno = false;

    g_gui_test_kbd_open_fails = false;
    g_gui_test_kbd_open_errno = 0;
    g_gui_test_mouse_open_fails = false;
    g_gui_test_mouse_open_errno = 0;
    g_gui_test_kbd_open_flags = 0;
    g_gui_test_mouse_open_flags = 0;

    g_gui_test_fail_bind_errno = 0;
    g_gui_test_bind_initial_modifiers = 0;
    g_gui_test_bind_generation = 1;

    g_gui_test_fail_display_state_errno = 0;
    g_gui_test_display_flags = GFX_DISPLAY_STATE_FOREGROUND;
    g_gui_test_display_generation = 1;

    g_gui_test_fail_lvgl_create_errno = 0;

    g_gui_test_fail_widget_registry = false;
    g_gui_test_fail_widget_observer = false;
    g_gui_test_fail_focus_observer = false;
    g_gui_test_focus_rebuild_count = 0;
    g_gui_test_fail_widget_create_countdown = 0;
    g_gui_test_fail_focus_register_countdown = 0;

    gui_test_device_reset(&g_gui_test_keyboard);
    gui_test_device_reset(&g_gui_test_mouse);

#ifndef GUI_INTEGRATION_TEST
    g_gui_test_fail_lv_display_create = false;
    g_gui_test_handler_delay = 20;

    g_gui_test_last_display = NULL;
    s_flush_count = 0;
#endif
}

void gui_test_set_now(uint64_t ms)
{
    g_gui_test_now_ms = ms;
}

uint64_t gui_test_get_now(void)
{
    return g_gui_test_now_ms;
}

void gui_test_advance_now(uint64_t ms)
{
    g_gui_test_now_ms += ms;
}

#ifndef GUI_INTEGRATION_TEST
void gui_test_queue_flush(const lv_area_t *area, const uint32_t *pixels)
{
    if (s_flush_count < MAX_QUEUED_FLUSHES) {
        if (area) {
            s_flush_queue[s_flush_count].area = *area;
        } else {
            memset(&s_flush_queue[s_flush_count].area, 0, sizeof(lv_area_t));
        }
        s_flush_queue[s_flush_count].pixels = pixels;
        s_flush_count++;
    }
}
#endif

/* ── Wrappers ─────────────────────────────────────────────────── */

extern void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (g_gui_test_intercept_calloc && g_gui_test_fail_calloc) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(nmemb, size);
}

extern void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (g_gui_test_intercept_mmap) {
        g_gui_test_counts.mmap++;
        if (g_gui_test_fail_mmap_n > 0) {
            g_gui_test_fail_mmap_n--;
            if (g_gui_test_fail_mmap_n == 0) {
                errno = ENOMEM;
                return MAP_FAILED;
            }
        }
        if (g_gui_test_mock_large_mmap && length > 64 * 1024 * 1024) {
            return __real_mmap(NULL, 4096, prot, flags | MAP_ANONYMOUS, -1, 0);
        }
    }
    return __real_mmap(addr, length, prot, flags, fd, offset);
}

extern int __real_munmap(void *addr, size_t length);
int __wrap_munmap(void *addr, size_t length)
{
    if (g_gui_test_intercept_mmap) {
        g_gui_test_counts.munmap++;
        if (addr == MAP_FAILED) {
            fprintf(stderr, "FATAL TEST FAILURE: munmap called on MAP_FAILED (%p)!\n", addr);
            abort();
        }
        if (g_gui_test_cleanup_change_errno) {
            errno = EBUSY;
        }
        if (g_gui_test_mock_large_mmap && length > 64 * 1024 * 1024) {
            return __real_munmap(addr, 4096);
        }
    }
    return __real_munmap(addr, length);
}

extern int __real_open(const char *pathname, int flags, ...);
int __wrap_open(const char *pathname, int flags, ...)
{
    if (g_gui_test_intercept_fb && pathname && strcmp(pathname, "/dev/fb") == 0) {
        if (g_gui_test_fail_fb_open) {
            errno = g_gui_test_fail_fb_open_errno ? g_gui_test_fail_fb_open_errno : ENOENT;
            return -1;
        }
        return 999;
    }
    if (pathname && strcmp(pathname, "/dev/keyboard") == 0) {
        g_gui_test_kbd_open_flags = flags;
        if (g_gui_test_kbd_open_fails) {
            errno = g_gui_test_kbd_open_errno ? g_gui_test_kbd_open_errno : ENOENT;
            return -1;
        }
        g_gui_test_counts.open++;
        return GUI_TEST_KEYBOARD_FD;
    }
    if (pathname && strcmp(pathname, "/dev/mouse") == 0) {
        g_gui_test_mouse_open_flags = flags;
        if (g_gui_test_mouse_open_fails) {
            errno = g_gui_test_mouse_open_errno ? g_gui_test_mouse_open_errno : ENOENT;
            return -1;
        }
        g_gui_test_counts.open++;
        return GUI_TEST_MOUSE_FD;
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    return __real_open(pathname, flags, mode);
}

extern ssize_t __real_read(int fd, void *buf, size_t count);
ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    if (fd == 999) {
        if (g_gui_test_fail_fb_read) {
            errno = g_gui_test_fail_fb_read_errno ? g_gui_test_fail_fb_read_errno : EIO;
            return -1;
        }
        if (g_gui_test_short_fb_read) {
            size_t copy_sz = sizeof(struct fb_info) > 4 ? sizeof(struct fb_info) - 4 : 1;
            if (count < copy_sz) copy_sz = count;
            memcpy(buf, &g_gui_test_fb_info, copy_sz);
            return (ssize_t)copy_sz;
        }
        size_t copy_sz = sizeof(struct fb_info);
        if (count < copy_sz) copy_sz = count;
        memcpy(buf, &g_gui_test_fb_info, copy_sz);
        return (ssize_t)copy_sz;
    }
    if (fd == GUI_TEST_KEYBOARD_FD) {
        g_gui_test_counts.read++;
        return gui_test_device_read(&g_gui_test_keyboard, buf, count);
    }
    if (fd == GUI_TEST_MOUSE_FD) {
        g_gui_test_counts.read++;
        return gui_test_device_read(&g_gui_test_mouse, buf, count);
    }
    return __real_read(fd, buf, count);
}

extern int __real_close(int fd);
int __wrap_close(int fd)
{
    if (fd == 999) {
        return 0;
    }
    if (fd == GUI_TEST_KEYBOARD_FD || fd == GUI_TEST_MOUSE_FD) {
        g_gui_test_counts.fd_close++;
        return 0;
    }
    return __real_close(fd);
}

extern int __real_clock_gettime(clockid_t clk_id, struct timespec *tp);
int __wrap_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    if (g_gui_test_intercept_clock && clk_id == CLOCK_MONOTONIC) {
        if (g_gui_test_fail_clock) {
            errno = g_gui_test_fail_clock_errno ? g_gui_test_fail_clock_errno : EIO;
            return -1;
        }
        if (g_gui_test_fail_clock_after_n > 0) {
            g_gui_test_fail_clock_after_n--;
            if (g_gui_test_fail_clock_after_n == 0) {
                errno = g_gui_test_fail_clock_errno ? g_gui_test_fail_clock_errno : EIO;
                return -1;
            }
        }
        if (tp) {
            tp->tv_sec = (time_t)(g_gui_test_now_ms / 1000ULL);
            tp->tv_nsec = (long)((g_gui_test_now_ms % 1000ULL) * 1000000ULL);
        }
        return 0;
    }
    return __real_clock_gettime(clk_id, tp);
}

static int s_mock_gfx_val = 0x42;

gfx_handle_t *__wrap_gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (g_gui_test_fail_gfx_open) {
        errno = g_gui_test_fail_gfx_open_errno ? g_gui_test_fail_gfx_open_errno : ENODEV;
        return NULL;
    }
    return (gfx_handle_t *)&s_mock_gfx_val;
}

gfx_info_t __wrap_gfx_get_info(const gfx_handle_t *h)
{
    (void)h;
    if (g_gui_test_fail_gfx_info) {
        errno = EINVAL;
        return (gfx_info_t){0};
    }
    if (g_gui_test_mismatch_gfx_info) {
        return g_gui_test_gfx_info_override;
    }
    return (gfx_info_t){
        .width = g_gui_test_fb_info.width,
        .height = g_gui_test_fb_info.height,
        .stride = g_gui_test_fb_info.width * 4,
        .format = GFX_FORMAT_RGB32
    };
}

int __wrap_gfx_acquire(gfx_handle_t *h)
{
    (void)h;
    g_gui_test_counts.acquire++;
    if (g_gui_test_fail_acquire_errno) {
        errno = g_gui_test_fail_acquire_errno;
        return -1;
    }
    return 0;
}

int __wrap_gfx_present(gfx_handle_t *h)
{
    (void)h;
    g_gui_test_counts.present++;
    if (g_gui_test_present_blocking_ms > 0) {
        g_gui_test_now_ms += g_gui_test_present_blocking_ms;
    }
    if (g_gui_test_fail_present_errno) {
        errno = g_gui_test_fail_present_errno;
        return -1;
    }
    return 0;
}

int __wrap_gfx_release(gfx_handle_t *h)
{
    (void)h;
    g_gui_test_counts.release++;
    if (g_gui_test_fail_release_errno) {
        errno = g_gui_test_fail_release_errno;
        return -1;
    }
    return 0;
}

void __wrap_gfx_close(gfx_handle_t *h)
{
    if (h) {
        g_gui_test_counts.close++;
    }
    if (g_gui_test_cleanup_change_errno) {
        errno = EBUSY;
    }
}

void __wrap_gfx_sprite_blit(gfx_handle_t *h, int32_t dst_x, int32_t dst_y,
                            const uint32_t *sprite, uint32_t stride,
                            uint32_t w, uint32_t h_, bool has_key, uint32_t key_color)
{
    (void)h; (void)dst_x; (void)dst_y; (void)sprite; (void)stride;
    (void)w; (void)h_; (void)has_key; (void)key_color;
}

/* ── The physical-input lease (GUI-2 Task 4/6) ─────────────────── */

/*
 * The host fixtures do NOT link the real libgfx: the mock IS the whole
 * gfx surface there (the GUI fixture link lines pull in the mock, the
 * GUI objects and the real LVGL archive — never libgfx.a).
 *
 * The __real_* entry points below are therefore LOCAL MODELS of the real
 * libgfx functions, not the real ones.  They model "there is no gfx
 * device behind this handle": the call fails with ENODEV.
 *
 * They are deliberately NOT permanently-empty weak stubs and they never
 * succeed: an empty stub would make the delegation path look like a
 * successful bind, which is exactly the false positive the registered
 * handle contract exists to prevent.
 */
int __real_gfx_bind_input(gfx_handle_t *h, int keyboard_fd, int mouse_fd,
                          gfx_input_state_t *out_state)
{
    (void)h; (void)keyboard_fd; (void)mouse_fd; (void)out_state;
    errno = ENODEV;
    return -1;
}

int __real_gfx_unbind_input(gfx_handle_t *h)
{
    (void)h;
    errno = ENODEV;
    return -1;
}

int __real_gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)
{
    (void)h; (void)out;
    errno = ENODEV;
    return -1;
}

/*
 * The ONE handle this mock issued: __wrap_gfx_open hands out exactly
 * &s_mock_gfx_val.  Only that handle is intercepted; every other handle
 * is handed back to the real implementation (the local model above).
 * The libgui input unit only ever binds the handle it obtained from
 * gfx_open(), so the registered path is the production path.
 */
static bool gui_test_gfx_handle_registered(gfx_handle_t *h)
{
    return h == (gfx_handle_t *)&s_mock_gfx_val;
}

int __wrap_gfx_bind_input(gfx_handle_t *h, int keyboard_fd, int mouse_fd,
                          gfx_input_state_t *out_state)
{
    if (!gui_test_gfx_handle_registered(h)) {
        /* Not our handle: hand it back to the real implementation. */
        return __real_gfx_bind_input(h, keyboard_fd, mouse_fd, out_state);
    }
    if (g_gui_test_fail_bind_errno) {
        errno = g_gui_test_fail_bind_errno;
        return -1;
    }
    /* 只统计**成功**的绑定：计数即“仍然有效的租约”数量。 */
    g_gui_test_counts.bind++;
    if (!out_state) {
        errno = EINVAL;
        return -1;
    }
    out_state->generation = g_gui_test_bind_generation;
    out_state->initial_modifiers = g_gui_test_bind_initial_modifiers;
    out_state->reserved = 0;
    return 0;
}

int __wrap_gfx_unbind_input(gfx_handle_t *h)
{
    if (!gui_test_gfx_handle_registered(h)) {
        return __real_gfx_unbind_input(h);
    }
    g_gui_test_counts.unbind++;
    return 0;
}

int __wrap_gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)
{
    if (!gui_test_gfx_handle_registered(h)) {
        return __real_gfx_get_display_state(h, out);
    }
    g_gui_test_counts.display_state++;
    if (g_gui_test_fail_display_state_errno) {
        errno = g_gui_test_fail_display_state_errno;
        return -1;
    }
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    out->generation = g_gui_test_display_generation;
    out->flags = g_gui_test_display_flags;
    out->reserved = 0;
    return 0;
}

#ifndef GUI_INTEGRATION_TEST
extern lv_display_t *__real_lv_display_create(int32_t hor_res, int32_t ver_res);
lv_display_t *__wrap_lv_display_create(int32_t hor_res, int32_t ver_res)
{
    if (g_gui_test_fail_lv_display_create) {
        return NULL;
    }
    lv_display_t *disp = __real_lv_display_create(hor_res, ver_res);
    g_gui_test_last_display = disp;
    return disp;
}

extern void __real_lv_display_flush_ready(lv_display_t *disp);
void __wrap_lv_display_flush_ready(lv_display_t *disp)
{
    g_gui_test_counts.ready++;
    __real_lv_display_flush_ready(disp);
}

uint32_t __wrap_lv_timer_handler(void)
{
    g_gui_test_counts.handler++;
    if (g_gui_test_last_display && s_flush_count > 0) {
        lv_display_flush_cb_t cb = g_gui_test_last_display->flush_cb;
        if (cb) {
            int count = s_flush_count;
            s_flush_count = 0;
            for (int i = 0; i < count; i++) {
                cb(g_gui_test_last_display, &s_flush_queue[i].area, (uint8_t *)s_flush_queue[i].pixels);
            }
        } else {
            s_flush_count = 0;
        }
    }
    return g_gui_test_handler_delay;
}
#endif
