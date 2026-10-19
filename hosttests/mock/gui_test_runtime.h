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

#ifndef GUI_INTEGRATION_TEST
extern bool g_gui_test_fail_lv_display_create;
extern uint32_t g_gui_test_handler_delay;

extern lv_display_t *g_gui_test_last_display;
#endif

#ifdef __cplusplus
}
#endif

#endif /* _GUI_TEST_RUNTIME_H */
