#ifndef _GUI_TEST_RUNTIME_H
#define _GUI_TEST_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <lvgl/lvgl.h>
#include <uapi/fb.h>
#include <gfx.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gui_test_counts {
    uint32_t handler;
    uint32_t acquire;
    uint32_t present;
    uint32_t release;
    uint32_t close;
    uint32_t ready;
    uint32_t mmap;
    uint32_t munmap;
    /* GUI-2 Task 6: the input lease and the raw device files */
    uint32_t open;          /* the raw device opens */
    uint32_t read;          /* the raw device reads */
    uint32_t fd_close;      /* the raw device closes */
    uint32_t bind;          /* GFX_BIND_INPUT */
    uint32_t unbind;        /* GFX_UNBIND_INPUT */
    uint32_t display_state; /* GFX_GET_DISPLAY_STATE */
    uint32_t lvgl_create;   /* the LVGL adapter create hook */
};

extern struct gui_test_counts g_gui_test_counts;

void gui_test_reset(void);
void gui_test_set_now(uint64_t ms);
uint64_t gui_test_get_now(void);
void gui_test_advance_now(uint64_t ms);
#ifndef GUI_INTEGRATION_TEST
void gui_test_queue_flush(const lv_area_t *area, const uint32_t *pixels);
#endif

/* Fault injection and mock configurations */
extern bool g_gui_test_intercept_calloc;
extern bool g_gui_test_fail_calloc;

extern bool g_gui_test_intercept_mmap;
extern int  g_gui_test_fail_mmap_n;
extern bool g_gui_test_mock_large_mmap;
extern bool g_gui_test_fail_munmap;

extern bool g_gui_test_intercept_fb;
extern bool g_gui_test_fail_fb_open;
extern int  g_gui_test_fail_fb_open_errno;
extern bool g_gui_test_fail_fb_read;
extern int  g_gui_test_fail_fb_read_errno;
extern bool g_gui_test_short_fb_read;
extern struct fb_info g_gui_test_fb_info;

extern bool g_gui_test_intercept_clock;
extern bool g_gui_test_fail_clock;
extern int  g_gui_test_fail_clock_errno;
extern int  g_gui_test_fail_clock_after_n;

extern bool g_gui_test_fail_gfx_open;
extern int  g_gui_test_fail_gfx_open_errno;
extern bool g_gui_test_fail_gfx_info;
extern bool g_gui_test_mismatch_gfx_info;
extern gfx_info_t g_gui_test_gfx_info_override;

extern int  g_gui_test_fail_acquire_errno;
extern int  g_gui_test_fail_present_errno;
extern uint32_t g_gui_test_present_blocking_ms;
extern int  g_gui_test_fail_release_errno;
extern bool g_gui_test_cleanup_change_errno;

/* ── GUI-2 Task 6: the raw input devices and the input lease ──── */

/* The fake descriptors handed out for /dev/keyboard and /dev/mouse. */
#define GUI_TEST_KEYBOARD_FD 900
#define GUI_TEST_MOUSE_FD    901

/* Device open fault injection. */
extern bool g_gui_test_kbd_open_fails;
extern int  g_gui_test_kbd_open_errno;
extern bool g_gui_test_mouse_open_fails;
extern int  g_gui_test_mouse_open_errno;
extern int  g_gui_test_kbd_open_flags;    /* the flags of the last /dev/keyboard open */
extern int  g_gui_test_mouse_open_flags;  /* the flags of the last /dev/mouse open */

/* The input lease (GFX_BIND_INPUT / GFX_UNBIND_INPUT). */
extern int  g_gui_test_fail_bind_errno;
extern uint32_t g_gui_test_bind_initial_modifiers;
extern uint64_t g_gui_test_bind_generation;

/* The display ownership state (GFX_GET_DISPLAY_STATE). */
extern int  g_gui_test_fail_display_state_errno;
extern uint32_t g_gui_test_display_flags;
extern uint64_t g_gui_test_display_generation;

/* The LVGL hook failure injection (the wrapper lives in the test TU). */
extern int  g_gui_test_fail_lvgl_create_errno;

#define GUI_TEST_DEVICE_MAX_OPS  16
#define GUI_TEST_DEVICE_MAX_DATA 2048

/* One scripted read outcome: either an error or a delivery of `len` bytes
 * taken from the device data buffer. */
typedef struct {
    int    err;   /* 0 = deliver data, >0 = the read fails with this errno */
    size_t len;   /* the bytes delivered by this read */
} gui_test_read_op_t;

typedef struct {
    gui_test_read_op_t ops[GUI_TEST_DEVICE_MAX_OPS];
    int                op_count;
    int                op_index;
    uint8_t            data[GUI_TEST_DEVICE_MAX_DATA];
    size_t             data_len;
    size_t             data_pos;
} gui_test_device_t;

extern gui_test_device_t g_gui_test_keyboard;
extern gui_test_device_t g_gui_test_mouse;

void gui_test_device_reset(gui_test_device_t *dev);
void gui_test_device_add_op(gui_test_device_t *dev, int err, size_t len);
void gui_test_device_add_data(gui_test_device_t *dev, const void *bytes, size_t len);
/* The bytes already delivered by the scripted reads. */
size_t gui_test_device_consumed(const gui_test_device_t *dev);

#ifndef GUI_INTEGRATION_TEST
extern bool g_gui_test_fail_lv_display_create;
extern uint32_t g_gui_test_handler_delay;

extern lv_display_t *g_gui_test_last_display;
#endif

#ifdef __cplusplus
}
#endif

#endif /* _GUI_TEST_RUNTIME_H */
