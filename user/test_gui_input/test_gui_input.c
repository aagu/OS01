/*
 * user/test_gui_input/test_gui_input.c — GUI-2 guest input interaction probe.
 *
 * The probe is the **evidence producer** of the GUI-2 "exclusive physical
 * input lease" plan: it runs *inside* OS01, takes the exclusive physical
 * input lease (the real /dev/keyboard + /dev/mouse devices, never synthetic
 * input), drives a fixed test UI and reports everything it observes over a
 * machine-checkable protocol on fd 1 / fd 2.
 *
 * Protocol (every line starts with the literal "[gui-input]"):
 *
 *   [gui-input] nonce=<hex> seq=<n> phase=<PHASE> ...
 *
 *   READY    pid width height + the target coordinates of every widget
 *   STATE    click_count slider scroll focus textarea (hex byte strings)
 *            + physical_key modifiers action (decimal) + sigint_seen
 *            + pointer_x pointer_y (hex) + pointer_seen
 *   ACK      ctl_seq pid command raw_pump      (control-file answer)
 *   RELEASED ctl_seq pid command raw_pump      (the owner's release answer)
 *   EXIT     result exit_code detail
 *   ERROR    errno detail
 *
 * The probe never reads the shared stdin: the runner drives it through the
 * serial shell (query the state, write the control file, wait).  Every
 * scenario is bounded and the control file is removed on exit.
 *
 * The private bridge headers (gui_input_private.h, gui_platform_private.h,
 * gui_lvgl_private.h) are the probe's *sanctioned per-app exception*, exactly
 * like user/test_lvgl/app.mk: they are not published in the sysroot and no
 * public API grows because of them.
 */

#include <gui/gui.h>

/* ── private bridge (probe-only exception; never published) ─────────── */
#include <gui_input_private.h>
#include <gui_platform_private.h>
#include <gui_lvgl_private.h>

#include <gfx.h>
#include <lvgl/lvgl.h>

/* The probe's nonce rule (shared with the host test). */
#include "gui_input_nonce.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Fixed protocol / scenario constants
 * ══════════════════════════════════════════════════════════════════════ */

#define PROTO_PREFIX       "[gui-input]"
#define PROTO_MAX_EXTRA    2048
#define PROTO_MAX_NONCE    (GUI_INPUT_NONCE_MAX_CHARS + 1)

/* The control channel: a plain file in /tmp, no FIFO and no new device.
 * The path is built by gui_input_nonce_ctl_path() from the validated hex
 * nonce, so it can only ever live in GUI_INPUT_CTL_DIR. */
#define CTL_MAX_RECORD     256
#define CTL_POLL_MS        10      /* re-open + re-read at most every 10 ms */

/* The owner scenario releases the view only after a still first frame:
 * this many consecutive steps without a new flush, and never later than
 * STILL_FRAME_WAIT_MS after READY (the safety valve). */
#define STILL_FRAMES_REQUIRED 3u
#define STILL_FRAME_WAIT_MS   2000u

/* Bounded scenario lifetimes (milliseconds). */
#define LIFE_INTERACTION   60000u
#define LIFE_ISOLATION     60000u
#define LIFE_HOLD          10000u
#define LIFE_FATAL         60000u
#define LIFE_OVERFLOW      10000u
#define LIFE_OWNER         60000u
#define LIFE_CRASH          60000u

/* Set 1 scan code of F12 (the fatal scenario waits for a real F12 DOWN). */
#define PS2_SCANCODE_F12   0x58u
/* Set 1 scan code of 'q' (the exit shortcut is Ctrl-Q). */
#define PS2_SCANCODE_Q     0x10u

/* Widget identities reported through the "focus" byte. */
#define WIDGET_NONE        0x00u
#define WIDGET_BUTTON      0x01u
#define WIDGET_SLIDER      0x02u
#define WIDGET_SCROLL      0x03u
#define WIDGET_TEXTAREA1   0x04u
#define WIDGET_TEXTAREA2   0x05u
#define WIDGET_DISABLED    0x06u

/* Fixed colours: the focus outline and the pointer cursor are painted by
 * libgui with these exact values so the runner can sample them from a
 * screenshot.  The probe only asserts that the constants it inherits are
 * the expected ones. */
#define PROBE_OUTLINE_COLOR 0x00FF00u
#define PROBE_CURSOR_FILL_COLOR    0x001A1C1Eu
#define PROBE_CURSOR_OUTLINE_COLOR 0x00FDFCFFu

/* ══════════════════════════════════════════════════════════════════════
 *  Small helpers
 * ══════════════════════════════════════════════════════════════════════ */

static volatile int sigint_seen = 0;

/*
 * The SIGINT handler observes — it never suppresses.
 *
 * It only sets the flag; it calls no logging and no allocation function
 * (both are forbidden inside a signal handler).  SIG_IGN is NOT used: a
 * TTY signal leak must stay visible, and the process group is never
 * changed.
 */
static void probe_sigint_handler(int signo)
{
    (void)signo;
    sigint_seen = 1;
}

static int now_ms(uint64_t *out_ms)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    *out_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    return 0;
}

/*
 * Hex encoding.
 *
 * The contract is explicit: a hex byte string is either written COMPLETELY
 * and NUL-terminated, or the encoder fails and leaves an EMPTY (still
 * NUL-terminated) string behind.  The buffer is never left uninitialised,
 * and every caller checks the return value — a payload that does not fit
 * is a bug, not something to paper over.
 *
 * The numeric payloads are fixed width (big-endian), so the runner never
 * has to guess the endianness.
 */

/*
 * The text payloads are bounded so that a STATE or an EXIT record always
 * fits in PROTO_MAX_EXTRA.  The truncation is deterministic: the FIRST
 * HEX_TEXT_MAX_CHARS characters are kept and the number of dropped
 * characters is reported in the "text_trunc" field.
 */
#define HEX_TEXT_MAX_CHARS  256
#define HEX_TEXT_MAX_BYTES  (2u * HEX_TEXT_MAX_CHARS + 1u)

/*
 * Writes len bytes as a NUL-terminated hex byte string.
 * Returns the number of hex characters written, or 0 when the payload does
 * not fit (the output is then set to the empty string).
 */
static size_t hex_encode(const unsigned char *data, size_t len, char *out, size_t out_len)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    size_t need = len * 2u;

    if (out == NULL || out_len == 0u) {
        return 0u;
    }
    if (out_len < need + 1u) {
        out[0] = '\0';       /* never leave the buffer uninitialised */
        return 0u;
    }
    for (i = 0; i < len; i++) {
        out[i * 2u] = digits[(data[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = digits[data[i] & 0x0Fu];
    }
    out[need] = '\0';
    return need;
}

/* Fixed width, big-endian.  Returns true when the full payload was
 * written; the buffers below are sized so that it always is, but the
 * result is checked all the same. */
static bool hex_encode_u32(uint32_t value, char *out, size_t out_len)
{
    unsigned char raw[4];

    raw[0] = (unsigned char)((value >> 24) & 0xFFu);
    raw[1] = (unsigned char)((value >> 16) & 0xFFu);
    raw[2] = (unsigned char)((value >> 8) & 0xFFu);
    raw[3] = (unsigned char)(value & 0xFFu);
    return hex_encode(raw, sizeof(raw), out, out_len) == (sizeof(raw) * 2u);
}

static bool hex_encode_i32(int32_t value, char *out, size_t out_len)
{
    return hex_encode_u32((uint32_t)value, out, out_len);
}

static bool hex_encode_u8(uint8_t value, char *out, size_t out_len)
{
    unsigned char raw[1];

    raw[0] = value;
    return hex_encode(raw, sizeof(raw), out, out_len) == (sizeof(raw) * 2u);
}

/*
 * Encodes at most HEX_TEXT_MAX_CHARS characters of the text.  *out_truncated
 * receives the number of source characters that were dropped (0 when the
 * whole text fitted).  Returns true when the payload was written.
 */
static bool hex_encode_text(const char *text, char *out, size_t out_len,
                             size_t *out_truncated)
{
    size_t len = (text != NULL) ? strlen(text) : 0u;
    size_t used = (len > (size_t)HEX_TEXT_MAX_CHARS) ? (size_t)HEX_TEXT_MAX_CHARS : len;

    if (out_truncated != NULL) {
        *out_truncated = len - used;
    }
    return hex_encode((const unsigned char *)(text != NULL ? text : ""), used,
                      out, out_len) == (used * 2u);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Probe state
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    SC_INTERACTION = 0,
    SC_ISOLATION,
    SC_HOLD,
    SC_FATAL,
    SC_OVERFLOW,
    SC_OWNER,
    SC_CRASH,
    SC_COUNT
} scenario_t;

static const char *const k_scenario_names[SC_COUNT] = {
    "interaction", "isolation", "hold", "fatal", "overflow", "owner",
    "crash"
};

typedef struct {
    char       nonce[PROTO_MAX_NONCE];
    scenario_t scenario;
    int        pid;
    uint32_t   seq;

    gui_platform_t *platform;
    gui_input_t    *input;

    /* The fixed test UI. */
    lv_obj_t *btn;
    lv_obj_t *slider;
    lv_obj_t *scroll_box;
    lv_obj_t *ta1;
    lv_obj_t *ta2;
    lv_obj_t *disabled_btn;

    /* The widget target coordinates (reported in READY). */
    int32_t btn_x, btn_y;
    int32_t slider_x, slider_y;
    int32_t scroll_x, scroll_y;
    int32_t ta1_x, ta1_y;
    int32_t ta2_x, ta2_y;

    /* The observed state. */
    uint32_t click_count;
    int32_t  slider_value;
    int32_t  scroll_value;
    uint8_t  focus_id;

    /* The last native key seen through the public key handler. */
    uint16_t last_physical_key;
    uint16_t last_modifiers;
    int      last_action;      /* -1 = no key yet */

    /* The last pointer position seen through the public pointer handler.
     * The pointer path is otherwise unobserved: the widget state (the
     * click counter) does not say WHERE the pointer was. */
    int32_t  pointer_x;
    int32_t  pointer_y;
    bool     pointer_seen;

    /* The control channel. */
    char     ctl_path[128];
    uint64_t last_ctl_poll_ms;
    uint32_t last_ctl_seq;
    uint32_t ctl_acks;
    uint32_t progress_acks;

    /* Scenario state. */
    bool     exit_requested;
    bool     ended_by_shortcut;
    bool     dispatch_paused;
    bool     step_paused;
    bool     owner_released;
    bool     owner_loss_seen;
    bool     fatal_seen;
    bool     overflow_seen;
    bool     state_emitted;
    char     last_state[PROTO_MAX_EXTRA];
    uint64_t start_ms;
    uint64_t limit_ms;
    uint32_t still_frames;
    uint32_t flush_count_seen;
} probe_t;

static probe_t g_probe;

/* ══════════════════════════════════════════════════════════════════════
 *  Protocol output
 * ══════════════════════════════════════════════════════════════════════ */

/* A diagnostic line: it never starts with the protocol marker, so the
 * runner's parser can never mistake it for a record. */
static void diag(const char *fmt, ...)
{
    char buf[PROTO_MAX_EXTRA];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[gui-input-probe] %s\n", buf);
    fflush(stderr);
}

/*
 * Emits one protocol record.  Returns false when the record does not fit:
 * a short ERROR record is emitted INSTEAD, so that the protocol sequence
 * stays contiguous and the caller can react by ending the scenario.
 */
static bool proto_emit(probe_t *p, const char *phase, const char *fmt, ...)
{
    char extra[PROTO_MAX_EXTRA];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(extra)) {
        diag("protocol record too long (%d bytes)", n);
        p->seq++;
        printf("\n%s nonce=%s seq=%u phase=ERROR errno=%d detail=record_too_long\n",
               PROTO_PREFIX, p->nonce, p->seq, EOVERFLOW);
        fflush(stdout);
        return false;
    }

    p->seq++;
    /*
     * The leading newline matters: the probe shares the console TTY with
     * the shell, and the shell's prompt is written WITHOUT a trailing
     * newline.  A background job's output would therefore continue the
     * prompt's line, and the record would no longer be anchored at the
     * start of a line.  The newline closes the prompt's line, so every
     * record is always anchored at the start of its own line.
     */
    printf("\n%s nonce=%s seq=%u phase=%s%s%s\n",
           PROTO_PREFIX, p->nonce, p->seq, phase,
           (extra[0] != '\0') ? " " : "", extra);
    fflush(stdout);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  The fixed test UI
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} rect_t;

static void btn_event_cb(lv_event_t *event)
{
    probe_t *p = (probe_t *)lv_event_get_user_data(event);

    if (p != NULL && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        p->click_count++;
    }
}

static uint8_t widget_id_of(const probe_t *p, const lv_obj_t *obj)
{
    if (obj == NULL) {
        return WIDGET_NONE;
    }
    if (obj == p->btn)         return WIDGET_BUTTON;
    if (obj == p->slider)      return WIDGET_SLIDER;
    if (obj == p->scroll_box)  return WIDGET_SCROLL;
    if (obj == p->ta1)         return WIDGET_TEXTAREA1;
    if (obj == p->ta2)         return WIDGET_TEXTAREA2;
    if (obj == p->disabled_btn) return WIDGET_DISABLED;
    return WIDGET_NONE;
}

static void build_ui(probe_t *p, uint32_t w, uint32_t h)
{
    const int32_t gap = 8;
    int32_t col_w;
    int32_t row_h;
    rect_t btn_r, slider_r, scroll_r, ta1_r, ta2_r, disabled_r;
    lv_obj_t *scr;
    lv_obj_t *label;
    int i;

    col_w = (int32_t)(((w > (uint32_t)(3u * gap)) ? (w - (uint32_t)(3u * gap)) : w) / 2u);
    if (col_w < 48) {
        col_w = (int32_t)w;
    }
    row_h = (int32_t)(((h > (uint32_t)(6u * gap)) ? (h - (uint32_t)(6u * gap)) : h) / 5u);
    if (row_h < 24) {
        row_h = 24;
    }

    btn_r.x = gap;
    btn_r.y = gap;
    btn_r.w = col_w;
    btn_r.h = row_h;

    slider_r.x = gap;
    slider_r.y = gap + row_h + gap;
    slider_r.w = col_w;
    slider_r.h = row_h;

    scroll_r.x = gap;
    scroll_r.y = gap + 2 * (row_h + gap);
    scroll_r.w = col_w;
    scroll_r.h = 2 * row_h + gap;

    ta1_r.x = gap;
    ta1_r.y = gap + 4 * (row_h + gap);
    ta1_r.w = col_w;
    ta1_r.h = row_h;

    disabled_r.x = gap + col_w + gap;
    disabled_r.y = gap;
    disabled_r.w = col_w;
    disabled_r.h = row_h;

    ta2_r.x = gap + col_w + gap;
    ta2_r.y = gap + 4 * (row_h + gap);
    ta2_r.w = col_w;
    ta2_r.h = row_h;

    scr = lv_display_get_screen_active(gui_platform_get_lv_display(p->platform));
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x00181825), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 1. The primary button: every click increments the click counter. */
    p->btn = lv_button_create(scr);
    lv_obj_set_pos(p->btn, btn_r.x, btn_r.y);
    lv_obj_set_size(p->btn, btn_r.w, btn_r.h);
    lv_obj_set_style_bg_color(p->btn, lv_color_hex(0x001A73E8), 0);
    lv_obj_set_style_bg_opa(p->btn, LV_OPA_COVER, 0);
    label = lv_label_create(p->btn);
    lv_label_set_text(label, "CLICK");
    lv_obj_center(label);
    lv_obj_add_event_cb(p->btn, btn_event_cb, LV_EVENT_CLICKED, p);

    /* 2. The slider. */
    p->slider = lv_slider_create(scr);
    lv_obj_set_pos(p->slider, slider_r.x, slider_r.y);
    lv_obj_set_size(p->slider, slider_r.w, slider_r.h);
    lv_slider_set_value(p->slider, 0, LV_ANIM_OFF);

    /* 3. The scrollable container: its content is taller than the box, so
     *    the wheel really has something to scroll.
     *
     *    The box is 2 * row_h + gap tall (348 px at 1440x900) and the
     *    labels are 24 px apart, so the content must reach past the box:
     *    12 lines ended at 284 px -- inside the box, which left the
     *    container with NO scroll range at all (lv_obj_get_scroll_bottom()
     *    was 0) and the wheel could not move anything.  24 lines end at
     *    570 px, which leaves 222 px of scroll range: several detents. */
    p->scroll_box = lv_obj_create(scr);
    lv_obj_set_pos(p->scroll_box, scroll_r.x, scroll_r.y);
    lv_obj_set_size(p->scroll_box, scroll_r.w, scroll_r.h);
    lv_obj_set_layout(p->scroll_box, LV_LAYOUT_NONE);
    lv_obj_set_scroll_dir(p->scroll_box, LV_DIR_VER);
    lv_obj_set_style_bg_color(p->scroll_box, lv_color_hex(0x00222233), 0);
    lv_obj_set_style_bg_opa(p->scroll_box, LV_OPA_COVER, 0);
    for (i = 0; i < 24; i++) {
        char line[32];
        int n = snprintf(line, sizeof(line), "SCROLL LINE %d", i);
        label = lv_label_create(p->scroll_box);
        if (n > 0) {
            lv_label_set_text(label, line);
        }
        lv_obj_set_pos(label, 6, 4 + i * 24);
    }

    /* 4. The first textarea: the primary, editable one. */
    p->ta1 = lv_textarea_create(scr);
    lv_obj_set_pos(p->ta1, ta1_r.x, ta1_r.y);
    lv_obj_set_size(p->ta1, ta1_r.w, ta1_r.h);
    lv_textarea_set_text(p->ta1, "");
    lv_textarea_set_one_line(p->ta1, true);

    /* 5. The second textarea: the secondary, editable one.  Both textareas
     *    are independent: the probe reports the content of each one, so the
     *    runner can verify which widget the typed text really landed in.
     *
     *    The second textarea is MULTI-LINE: LVGL filters the newline in
     *    one-line mode (it reports LV_EVENT_READY instead of inserting it),
     *    so only a multi-line widget can show what the runner asks it to
     *    hold.  The first one stays one-line, so the two are directly
     *    comparable. */
    p->ta2 = lv_textarea_create(scr);
    lv_obj_set_pos(p->ta2, ta2_r.x, ta2_r.y);
    lv_obj_set_size(p->ta2, ta2_r.w, ta2_r.h);
    lv_textarea_set_text(p->ta2, "");
    lv_textarea_set_one_line(p->ta2, false);

    /* 6. The disabled button: it must never become the focus target. */
    p->disabled_btn = lv_button_create(scr);
    lv_obj_set_pos(p->disabled_btn, disabled_r.x, disabled_r.y);
    lv_obj_set_size(p->disabled_btn, disabled_r.w, disabled_r.h);
    lv_obj_set_style_bg_color(p->disabled_btn, lv_color_hex(0x00333344), 0);
    lv_obj_set_style_bg_opa(p->disabled_btn, LV_OPA_COVER, 0);
    lv_obj_add_state(p->disabled_btn, LV_STATE_DISABLED);
    label = lv_label_create(p->disabled_btn);
    lv_label_set_text(label, "DISABLED");
    lv_obj_center(label);

    /* Status labels (read-only, never focusable). */
    label = lv_label_create(scr);
    lv_label_set_text(label, "GUI-2 INPUT PROBE");
    lv_obj_set_pos(label, gap + col_w + gap, gap + row_h + gap);

    /* The reported target coordinates are the *centres* of the widgets. */
    p->btn_x = btn_r.x + btn_r.w / 2;
    p->btn_y = btn_r.y + btn_r.h / 2;
    p->slider_x = slider_r.x + slider_r.w / 2;
    p->slider_y = slider_r.y + slider_r.h / 2;
    p->scroll_x = scroll_r.x + scroll_r.w / 2;
    p->scroll_y = scroll_r.y + scroll_r.h / 2;
    p->ta1_x = ta1_r.x + ta1_r.w / 2;
    p->ta1_y = ta1_r.y + ta1_r.h / 2;
    p->ta2_x = ta2_r.x + ta2_r.w / 2;
    p->ta2_y = ta2_r.y + ta2_r.h / 2;
}

/*
 * The private focus registration order is explicit and fixed:
 *
 *   1. the primary button
 *   2. the slider
 *   3. the scrollable container
 *   4. the first textarea
 *   5. the second textarea
 *
 * The disabled button is deliberately NOT registered: a disabled widget must
 * never become a focus target.
 */
static int register_focus(probe_t *p)
{
    if (gui_input_focus_add(p->input, p->btn) != 0) {
        return -1;
    }
    if (gui_input_focus_add(p->input, p->slider) != 0) {
        return -1;
    }
    if (gui_input_focus_add(p->input, p->scroll_box) != 0) {
        return -1;
    }
    if (gui_input_focus_add(p->input, p->ta1) != 0) {
        return -1;
    }
    if (gui_input_focus_add(p->input, p->ta2) != 0) {
        return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Observation
 * ══════════════════════════════════════════════════════════════════════ */

static void observe_state(probe_t *p)
{
    lv_obj_t *focus = gui_input_focus_current(p->input);

    p->slider_value = lv_slider_get_value(p->slider);
    p->scroll_value = lv_obj_get_scroll_y(p->scroll_box);
    p->focus_id = widget_id_of(p, focus);
}

/*
 * Builds the STATE payload.  Returns false when a payload could not be
 * encoded or the record does not fit — the caller MUST react (the record
 * is not emitted at all in that case, so the protocol sequence stays
 * contiguous).
 */
static bool build_state_fields(probe_t *p, char *out, size_t out_len,
                               const char *extra)
{
    char click_hex[16];
    char slider_hex[16];
    char scroll_hex[16];
    char focus_hex[8];
    char pointer_x_hex[16];
    char pointer_y_hex[16];
    char ta1_hex[HEX_TEXT_MAX_BYTES];
    char ta2_hex[HEX_TEXT_MAX_BYTES];
    size_t trunc1 = 0;
    size_t trunc2 = 0;
    int n;

    out[0] = '\0';

    if (!hex_encode_u32(p->click_count, click_hex, sizeof(click_hex)) ||
        !hex_encode_i32(p->slider_value, slider_hex, sizeof(slider_hex)) ||
        !hex_encode_i32(p->scroll_value, scroll_hex, sizeof(scroll_hex)) ||
        !hex_encode_u8(p->focus_id, focus_hex, sizeof(focus_hex)) ||
        !hex_encode_i32(p->pointer_x, pointer_x_hex, sizeof(pointer_x_hex)) ||
        !hex_encode_i32(p->pointer_y, pointer_y_hex, sizeof(pointer_y_hex)) ||
        !hex_encode_text(lv_textarea_get_text(p->ta1), ta1_hex, sizeof(ta1_hex), &trunc1) ||
        !hex_encode_text(lv_textarea_get_text(p->ta2), ta2_hex, sizeof(ta2_hex), &trunc2)) {
        return false;
    }

    n = snprintf(out, out_len,
                 "click_count=%s slider=%s scroll=%s focus=%s textarea=%s textarea2=%s "
                 "physical_key=%u modifiers=%u action=%d sigint_seen=%d text_trunc=%u "
                 "pointer_x=%s pointer_y=%s pointer_seen=%d%s%s",
                 click_hex, slider_hex, scroll_hex, focus_hex, ta1_hex, ta2_hex,
                 (unsigned)p->last_physical_key, (unsigned)p->last_modifiers,
                 p->last_action, sigint_seen ? 1 : 0,
                 (unsigned)(trunc1 + trunc2),
                 pointer_x_hex, pointer_y_hex, p->pointer_seen ? 1 : 0,
                 (extra != NULL && extra[0] != '\0') ? " " : "",
                 (extra != NULL) ? extra : "");
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/*
 * Emit a STATE record only when the observed state actually changed: the
 * click counter, the slider value, the scroll offset, the focus target, the
 * textarea content, the last native key or the SIGINT flag.
 */
static bool emit_state_if_changed(probe_t *p)
{
    char fields[PROTO_MAX_EXTRA];

    observe_state(p);
    if (!build_state_fields(p, fields, sizeof(fields), "")) {
        return false;
    }
    if (!p->state_emitted || strcmp(fields, p->last_state) != 0) {
        if (!proto_emit(p, "STATE", "%s", fields)) {
            return false;
        }
        snprintf(p->last_state, sizeof(p->last_state), "%s", fields);
        p->state_emitted = true;
    }
    return true;
}

/*
 * The fatal scenario must PROVE that the LVGL assertion really fired.
 *
 * The guest's global LV_ASSERT_HANDLER is `_exit(1)`: it reports nothing,
 * so the only evidence was "the process died" — any crash after the
 * trigger record would have passed.  This handler REPORTS the assertion
 * first (a distinct "lvgl_assert=1" field in a STATE record) and only
 * then exits, so the runner can assert a POSITIVE fact.
 *
 * The macro is expanded in THIS translation unit only: the LVGL library
 * keeps the guest's own handler.
 */
static void probe_report_lvgl_assert(void)
{
    probe_t *p = &g_probe;
    char fields[PROTO_MAX_EXTRA];

    /* The control file must not outlive the probe. */
    if (p->ctl_path[0] != '\0') {
        unlink(p->ctl_path);
    }
    if (build_state_fields(p, fields, sizeof(fields), "lvgl_assert=1")) {
        (void)proto_emit(p, "STATE", "%s", fields);
    }
    fflush(stdout);
    _exit(1);
}

#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER do { probe_report_lvgl_assert(); } while (0);

/* ══════════════════════════════════════════════════════════════════════
 *  The native key / pointer handlers
 * ══════════════════════════════════════════════════════════════════════ */

static bool scenario_honours_ctrl_q(scenario_t scenario);

static bool is_ctrl_q(const gui_key_event_t *ev)
{
    if (ev->action != GUI_KEY_DOWN) {
        return false;
    }
    if (ev->physical_key != PS2_SCANCODE_Q) {
        return false;
    }
    return (ev->modifiers & (uint16_t)(GUI_MOD_LCTRL | GUI_MOD_RCTRL)) != 0;
}

static bool key_handler(const gui_key_event_t *ev, void *userdata)
{
    probe_t *p = (probe_t *)userdata;

    if (p == NULL || ev == NULL) {
        return false;
    }

    p->last_physical_key = ev->physical_key;
    p->last_modifiers = ev->modifiers;
    p->last_action = (int)ev->action;

    /* The fatal scenario waits for a **real** PS/2 F12 DOWN. */
    if (p->scenario == SC_FATAL && ev->action == GUI_KEY_DOWN &&
        ev->physical_key == PS2_SCANCODE_F12) {
        p->fatal_seen = true;
        return true;    /* consumed: it must not reach the widget path */
    }

    /* Ctrl-Q is the exit shortcut.  It is always consumed (so it can never
     * leak into a textarea) but only *honoured* where the scenario allows
     * an exit input. */
    if (is_ctrl_q(ev)) {
        if (scenario_honours_ctrl_q(p->scenario)) {
            p->exit_requested = true;
        }
        return true;
    }

    return false;
}

static void pointer_handler(const gui_pointer_event_t *ev, void *userdata)
{
    probe_t *p = (probe_t *)userdata;

    if (!p || !ev) {
        return;
    }
    /* The pointer path is read-only here: the widget state (the click
     * counter) is the observable, not the raw coordinates — but the
     * POSITION is reported too, so the runner can assert that the
     * pointer really is where it moved it. */
    p->pointer_x = ev->x;
    p->pointer_y = ev->y;
    p->pointer_seen = true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  The control channel
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * The control record is exactly one line:
 *
 *   nonce=<hex> ctl_seq=<n> command=<COMMAND>\n
 *
 * The trailing newline is what makes a record *complete*: a partially
 * written file is simply waited out for the next round.
 *
 * Return codes: 0 = parsed, 1 = incomplete (wait), 2 = wrong nonce,
 * -1 = malformed (error).
 */
#define CTL_RC_OK        0
#define CTL_RC_PARTIAL   1
#define CTL_RC_BAD_NONCE 2
#define CTL_RC_MALFORMED (-1)

static bool ctl_is_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int parse_ctl_record(probe_t *p, const char *text, size_t len,
                            uint32_t *out_ctl_seq, char *out_command,
                            size_t command_len)
{
    const char *cursor = text;
    const char *end = text + len;
    char nonce[PROTO_MAX_NONCE];
    char command[64];
    unsigned long ctl_seq = 0;
    bool have_nonce = false;
    bool have_seq = false;
    bool have_command = false;

    nonce[0] = '\0';
    command[0] = '\0';

    while (cursor < end) {
        const char *key;
        const char *value;
        size_t key_len;
        size_t value_len;

        while (cursor < end && ctl_is_sep(*cursor)) {
            cursor++;
        }
        if (cursor >= end) {
            break;
        }

        key = cursor;
        while (cursor < end && !ctl_is_sep(*cursor) && *cursor != '=') {
            cursor++;
        }
        key_len = (size_t)(cursor - key);
        if (cursor >= end || *cursor != '=') {
            return CTL_RC_MALFORMED;      /* malformed token */
        }
        cursor++;           /* skip '=' */
        value = cursor;
        while (cursor < end && !ctl_is_sep(*cursor)) {
            cursor++;
        }
        value_len = (size_t)(cursor - value);

        if (key_len == 5u && memcmp(key, "nonce", 5u) == 0) {
            if (value_len >= sizeof(nonce)) {
                return CTL_RC_MALFORMED;
            }
            memcpy(nonce, value, value_len);
            nonce[value_len] = '\0';
            have_nonce = true;
        } else if (key_len == 7u && memcmp(key, "ctl_seq", 7u) == 0) {
            size_t i;
            if (value_len == 0u || value_len > 9u) {
                return CTL_RC_MALFORMED;
            }
            for (i = 0; i < value_len; i++) {
                if (value[i] < '0' || value[i] > '9') {
                    return CTL_RC_MALFORMED;
                }
            }
            ctl_seq = strtoul(value, NULL, 10);
            have_seq = true;
        } else if (key_len == 7u && memcmp(key, "command", 7u) == 0) {
            if (value_len >= sizeof(command)) {
                return CTL_RC_MALFORMED;
            }
            memcpy(command, value, value_len);
            command[value_len] = '\0';
            have_command = true;
        } else {
            return CTL_RC_MALFORMED;      /* unknown field */
        }
    }

    if (!have_nonce || !have_seq || !have_command) {
        return CTL_RC_PARTIAL;            /* incomplete: wait for the next round */
    }
    if (strcmp(nonce, p->nonce) != 0) {
        return CTL_RC_BAD_NONCE;
    }
    if (ctl_seq == 0u) {
        return CTL_RC_MALFORMED;
    }
    if (strlen(command) + 1u > command_len) {
        return CTL_RC_MALFORMED;
    }

    memcpy(out_command, command, strlen(command) + 1u);
    *out_ctl_seq = (uint32_t)ctl_seq;
    return CTL_RC_OK;
}

static bool ack_control(probe_t *p, uint32_t ctl_seq, const char *command,
                        const char *phase, int raw_pump)
{
    p->ctl_acks++;
    if (strcmp(command, "PROGRESS") == 0) {
        p->progress_acks++;
    }
    return proto_emit(p, phase, "ctl_seq=%u pid=%d command=%s raw_pump=%d",
                      ctl_seq, p->pid, command, raw_pump);
}

/*
 * A known command that does not apply to the running scenario.  It is NOT
 * a protocol error: the brief makes only three things hard errors (an
 * unknown command, a nonce error and out-of-order numbering).  The probe
 * answers with a normal ACK that carries applicable=0, so a runner that
 * sends RELEASE_VIEW generically does not kill the probe.
 */
static bool ack_not_applicable(probe_t *p, uint32_t ctl_seq, const char *command)
{
    p->ctl_acks++;
    return proto_emit(p, "ACK",
                      "ctl_seq=%u pid=%d command=%s raw_pump=%d applicable=0",
                      ctl_seq, p->pid, command, p->dispatch_paused ? 0 : 1);
}

/*
 * Execute one control record.  It returns 0 on success, 1 when the record
 * must be retried in a later round (the frame is not still yet) and -1 on
 * a protocol error.
 */
static int execute_control(probe_t *p, uint32_t ctl_seq, const char *command,
                          uint64_t now)
{
    if (strcmp(command, "SNAPSHOT") == 0) {
        char fields[PROTO_MAX_EXTRA];

        observe_state(p);
        if (!build_state_fields(p, fields, sizeof(fields), "")) {
            errno = EOVERFLOW;
            return -1;
        }
        if (!ack_control(p, ctl_seq, command, "ACK", p->dispatch_paused ? 0 : 1)) {
            return -1;
        }
        if (!proto_emit(p, "STATE", "%s", fields)) {
            return -1;
        }
        snprintf(p->last_state, sizeof(p->last_state), "%s", fields);
        p->state_emitted = true;
        return 0;
    }

    if (strcmp(command, "PROGRESS") == 0) {
        if (!ack_control(p, ctl_seq, command, "ACK", p->dispatch_paused ? 0 : 1)) {
            return -1;
        }
        return 0;
    }

    if (strcmp(command, "RELEASE_VIEW") == 0) {
        if (p->scenario != SC_OWNER && p->scenario != SC_HOLD) {
            /* Releasing the view would destroy the premise of the other
             * scenarios, so the command does not apply there: it is
             * acknowledged, not refused. */
            if (!ack_not_applicable(p, ctl_seq, command)) {
                return -1;
            }
            return 0;
        }
        /*
         * The owner scenario releases the view only after a still first
         * frame.  Until then the record is retried in a later round (it is
         * not consumed, so it is still executed exactly once).  The safety
         * valve keeps the scenario from hanging if the screen never settles.
         */
        if (p->scenario == SC_OWNER && !p->owner_released &&
            p->still_frames < STILL_FRAMES_REQUIRED &&
            (now - p->start_ms) <= STILL_FRAME_WAIT_MS) {
            return 1;
        }
        /*
         * The test-only release: it goes through the probe's own private
         * platform header.  The platform keeps its "acquired" record, so
         * the next step detects the loss of the ownership.  No public
         * release API is added and the gfx exclusivity rules are not
         * touched.
         */
        if (gfx_release(p->platform->gfx) != 0) {
            return -1;
        }
        p->owner_released = true;
        p->dispatch_paused = true;
        p->step_paused = true;
        if (!ack_control(p, ctl_seq, command, "RELEASED", 0)) {
            return -1;
        }
        return 0;
    }

    if (strcmp(command, "RESUME_STEP") == 0) {
        if (p->scenario != SC_OWNER && p->scenario != SC_HOLD) {
            /* Not applicable here: acknowledged, not refused. */
            if (!ack_not_applicable(p, ctl_seq, command)) {
                return -1;
            }
            return 0;
        }
        p->dispatch_paused = false;
        p->step_paused = false;
        if (!ack_control(p, ctl_seq, command, "ACK", 1)) {
            return -1;
        }
        return 0;
    }

    errno = EINVAL;         /* unknown command */
    return -1;
}

/*
 * Poll the control file.  It is re-opened and re-read completely at most
 * every CTL_POLL_MS milliseconds; the same ctl_seq is executed only once.
 */
static int poll_control(probe_t *p, uint64_t now)
{
    char buffer[CTL_MAX_RECORD];
    size_t got;
    size_t pos = 0;
    ssize_t raw;
    int fd;

    if (now - p->last_ctl_poll_ms < (uint64_t)CTL_POLL_MS) {
        return 0;
    }
    p->last_ctl_poll_ms = now;

    fd = open(p->ctl_path, O_RDONLY);
    if (fd < 0) {
        return 0;           /* no control file yet: nothing to do */
    }
    raw = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (raw <= 0) {
        return 0;           /* empty or transient: wait for the next round */
    }
    got = (size_t)raw;
    buffer[got] = '\0';

    /* Every complete line is one control record; a trailing partial line
     * is simply waited out for the next round. */
    while (pos < got) {
        char command[64];
        uint32_t ctl_seq = 0;
        size_t line_end = pos;
        int rc;

        while (line_end < got && buffer[line_end] != '\n') {
            line_end++;
        }
        if (line_end >= got) {
            break;          /* incomplete trailing line: wait */
        }

        rc = parse_ctl_record(p, buffer + pos, line_end - pos,
                              &ctl_seq, command, sizeof(command));
        pos = line_end + 1u;

        if (rc == CTL_RC_PARTIAL) {
            continue;       /* incomplete: wait for the next round */
        }
        if (rc == CTL_RC_BAD_NONCE) {
            errno = EPROTO;
            return -1;
        }
        if (rc != CTL_RC_OK) {
            errno = EINVAL;
            return -1;
        }

        if (ctl_seq == p->last_ctl_seq) {
            continue;       /* already executed: execute it only once */
        }
        if (ctl_seq < p->last_ctl_seq) {
            errno = EPROTO; /* out-of-order numbering */
            return -1;
        }

        rc = execute_control(p, ctl_seq, command, now);
        if (rc < 0) {
            return -1;
        }
        if (rc > 0) {
            continue;       /* retried in a later round, not consumed yet */
        }
        p->last_ctl_seq = ctl_seq;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scenario helpers
 * ══════════════════════════════════════════════════════════════════════ */

static uint64_t scenario_lifetime(scenario_t scenario)
{
    switch (scenario) {
    case SC_INTERACTION: return LIFE_INTERACTION;
    case SC_ISOLATION:   return LIFE_ISOLATION;
    case SC_HOLD:        return LIFE_HOLD;
    case SC_FATAL:       return LIFE_FATAL;
    case SC_OVERFLOW:    return LIFE_OVERFLOW;
    case SC_OWNER:       return LIFE_OWNER;
    case SC_CRASH:       return LIFE_CRASH;
    default:             return LIFE_INTERACTION;
    }
}

static bool scenario_honours_ctrl_q(scenario_t scenario)
{
    return scenario == SC_INTERACTION || scenario == SC_ISOLATION
        || scenario == SC_CRASH;
}

/* Emits the EXIT record.  Returns false when the record could not be
 * built; the caller MUST react (the EXIT is the last record of the
 * stream, so a failure ends the scenario with an ERROR instead). */
static bool emit_exit(probe_t *p, const char *result, int exit_code, const char *detail)
{
    char ta1_hex[HEX_TEXT_MAX_BYTES];
    char ta2_hex[HEX_TEXT_MAX_BYTES];
    char click_hex[16];
    char focus_hex[8];
    const char *text = lv_textarea_get_text(p->ta1);
    size_t trunc1 = 0;
    size_t trunc2 = 0;
    int n;

    if (!hex_encode_u32(p->click_count, click_hex, sizeof(click_hex)) ||
        !hex_encode_u8(p->focus_id, focus_hex, sizeof(focus_hex)) ||
        !hex_encode_text(text, ta1_hex, sizeof(ta1_hex), &trunc1) ||
        !hex_encode_text(lv_textarea_get_text(p->ta2), ta2_hex, sizeof(ta2_hex), &trunc2)) {
        return false;
    }

    n = snprintf(NULL, 0,
                 "result=%s exit_code=%d detail=%s textarea=%s textarea2=%s "
                 "click_count=%s focus=%s sigint_seen=%d ctl_acks=%u progress_acks=%u "
                 "recorded_bytes=%u text_trunc=%u",
                 result, exit_code, detail, ta1_hex, ta2_hex, click_hex, focus_hex,
                 sigint_seen ? 1 : 0, p->ctl_acks, p->progress_acks,
                 (unsigned)((text != NULL) ? strlen(text) : 0u),
                 (unsigned)(trunc1 + trunc2));
    if (n < 0 || (size_t)n >= (size_t)PROTO_MAX_EXTRA) {
        return false;
    }

    return proto_emit(p, "EXIT",
                       "result=%s exit_code=%d detail=%s textarea=%s textarea2=%s "
                       "click_count=%s focus=%s sigint_seen=%d ctl_acks=%u progress_acks=%u "
                       "recorded_bytes=%u text_trunc=%u",
                       result, exit_code, detail, ta1_hex, ta2_hex, click_hex, focus_hex,
                       sigint_seen ? 1 : 0, p->ctl_acks, p->progress_acks,
                       (unsigned)((text != NULL) ? strlen(text) : 0u),
                       (unsigned)(trunc1 + trunc2));
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

static void usage(void)
{
    fprintf(stderr,
            "[gui-input-probe] usage: test_gui_input --nonce <hex> "
            "--scenario <interaction|isolation|hold|fatal|overflow|owner|crash>\n");
}

static int fail(int code, int err, const char *detail)
{
    char text[PROTO_MAX_EXTRA];

    snprintf(text, sizeof(text), "errno=%d detail=%s", err, detail);
    /* Every call site runs after the nonce has been validated and copied,
     * so the record always carries a usable nonce. */
    proto_emit(&g_probe, "ERROR", "%s", text);
    return code;
}

int main(int argc, char **argv)
{
    probe_t *p = &g_probe;
    gui_config_t cfg;
    gui_input_config_t icfg;
    const char *nonce = NULL;
    const char *scenario_name = NULL;
    uint64_t now = 0;
    struct pollfd fds[2];
    int nfds;
    bool ready = false;
    bool present_seen = false;
    int i;
    int rc = 0;

    memset(p, 0, sizeof(*p));
    p->last_action = -1;
    p->pid = (int)getpid();

    /* ---- arguments (invalid arguments exit 2) ---------------------- */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nonce") == 0 && i + 1 < argc) {
            nonce = argv[++i];
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario_name = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (nonce == NULL || scenario_name == NULL) {
        usage();
        return 2;
    }
    /*
     * The nonce must be hex: it is interpolated into the control file's
     * path, so a path separator, a whitespace or a shell metacharacter
     * must be impossible by construction.  An invalid nonce is an invalid
     * argument, so the probe exits 2 without touching anything.
     */
    if (!gui_input_nonce_is_valid(nonce)) {
        usage();
        return 2;
    }
    for (i = 0; i < SC_COUNT; i++) {
        if (strcmp(scenario_name, k_scenario_names[i]) == 0) {
            p->scenario = (scenario_t)i;
            break;
        }
    }
    if (i == SC_COUNT) {
        usage();
        return 2;
    }
    memcpy(p->nonce, nonce, strlen(nonce) + 1u);

    /* The control path is fixed: /tmp/gui-input-<hexnonce>.ctl */
    if (gui_input_nonce_ctl_path(p->ctl_path, sizeof(p->ctl_path), p->nonce) != 0) {
        usage();
        return 2;
    }

    diag("scenario=%s nonce=%s pid=%d ctl=%s", scenario_name, p->nonce, p->pid,
         p->ctl_path);

    /* ---- the SIGINT observer -------------------------------------- */
    if (signal(SIGINT, probe_sigint_handler) == SIG_ERR) {
        rc = fail(1, errno, "signal"); goto cleanup;
    }

    /* ---- the platform (screen mode: real devices) ----------------- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = GUI_MODE_ONSCREEN;
    cfg.width = 0;
    cfg.height = 0;
    cfg.buffer_lines = 40;

    p->platform = gui_platform_create(&cfg);
    if (p->platform == NULL) {
        rc = fail(1, errno, "gui_platform_create"); goto cleanup;
    }

    /*
     * The physical input lease: real devices only, mouse mandatory.
     * Synthetic input, the control file and stdin text can never stand in
     * for a physical key.
     */
    memset(&icfg, 0, sizeof(icfg));
    icfg.mode = GUI_INPUT_DEVICES;
    icfg.require_mouse = true;

    p->input = gui_input_create(p->platform, &icfg);
    if (p->input == NULL) {
        rc = fail(1, errno, "gui_input_create"); goto cleanup;
    }
    if (!gui_input_has_mouse(p->input)) {
        rc = fail(1, ENODEV, "no_mouse"); goto cleanup;
    }

    gui_input_set_key_handler(p->input, key_handler, p);
    gui_input_set_pointer_handler(p->input, pointer_handler, p);

    /* ---- the fixed test UI ----------------------------------------- */
    build_ui(p, gui_platform_get_width(p->platform),
             gui_platform_get_height(p->platform));
    if (register_focus(p) != 0) {
        rc = fail(1, errno, "focus_add"); goto cleanup;
    }

    /* The fixed outline / cursor colours come from libgui. */
    if (GUI_INPUT_FOCUS_OUTLINE_COLOR != PROBE_OUTLINE_COLOR ||
        GUI_INPUT_CURSOR_FILL_COLOR != PROBE_CURSOR_FILL_COLOR ||
        GUI_INPUT_CURSOR_OUTLINE_COLOR != PROBE_CURSOR_OUTLINE_COLOR) {
        rc = fail(1, EINVAL, "focus_outline_or_cursor_color"); goto cleanup;
    }

    if (now_ms(&p->start_ms) != 0) {
        rc = fail(1, errno, "clock"); goto cleanup;
    }
    p->limit_ms = p->start_ms + scenario_lifetime(p->scenario);
    p->last_ctl_poll_ms = 0;

    /* ---- the first step / present ---------------------------------- */
    /*
     * READY may only be emitted after the acquire + bind (done by
     * gui_input_create) **and** the first step/present have succeeded.
     *
     * A frame is presented inside the same step that flushed it: the flush
     * callback raises pending_present and gui_platform_step presents it
     * right after lv_timer_handler().  A step that returned GUI_STATUS_OK
     * and raised the flush counter therefore presented a frame.
     */
    while (!present_seen) {
        uint32_t delay_ms = 20;
        uint32_t flush_before;
        gui_status_t status;

        if (now_ms(&now) != 0) {
            rc = fail(1, errno, "clock"); goto cleanup;
        }
        if (now - p->start_ms > 5000u) {
            rc = fail(1, ETIMEDOUT, "no_first_present"); goto cleanup;
        }
        flush_before = p->platform->flush_count;
        status = gui_platform_step(p->platform, &delay_ms);
        if (status != GUI_STATUS_OK) {
            if (status != GUI_STATUS_EAGAIN) {
                rc = fail(1, errno, "first_step"); goto cleanup;
            }
        } else if (p->platform->flush_count > flush_before) {
            present_seen = true;
        }
        if (delay_ms > 100u) {
            delay_ms = 100u;
        }
        poll(NULL, 0, (int)delay_ms);
    }

    /* ---- READY ------------------------------------------------------ */
    if (!proto_emit(p, "READY",
                    "pid=%d width=%u height=%u "
                    "btn_x=%d btn_y=%d slider_x=%d slider_y=%d "
                    "scroll_x=%d scroll_y=%d ta1_x=%d ta1_y=%d ta2_x=%d ta2_y=%d",
                    p->pid, gui_platform_get_width(p->platform),
                    gui_platform_get_height(p->platform),
                    p->btn_x, p->btn_y, p->slider_x, p->slider_y,
                    p->scroll_x, p->scroll_y, p->ta1_x, p->ta1_y, p->ta2_x, p->ta2_y)) {
        rc = fail(1, EOVERFLOW, "ready_record"); goto cleanup;
    }
    ready = true;

    /* The overflow scenario pauses the raw device pump from READY on; the
     * step, the error check and the control channel stay alive. */
    if (p->scenario == SC_OVERFLOW) {
        p->dispatch_paused = true;
    }

    /* ---- the main loop ---------------------------------------------- */
    while (ready) {
        uint32_t delay_ms = 10;
        int timeout_ms;

        if (now_ms(&now) != 0) {
            rc = fail(1, errno, "clock");
            break;
        }

        /* The control file is polled at most every 10 ms. */
        if (poll_control(p, now) != 0) {
            rc = fail(1, errno, "control");
            break;
        }

        if (p->exit_requested) {
            p->ended_by_shortcut = true;
            break;
        }

        /* The raw device pump. */
        if (!p->dispatch_paused) {
            nfds = gui_input_get_pollfds(p->input, fds);
            if (nfds < 0) {
                rc = fail(1, errno, "get_pollfds");
                break;
            }
        } else if (p->scenario == SC_OVERFLOW) {
            /*
             * The overflow scenario never dispatches: the ACTIVE keyboard
             * queue has to REALLY fill up, and every dispatch would drain
             * it.  It only WATCHES the devices here -- a poll is not a
             * read, so it never consumes a byte -- so that the POLLERR
             * the kernel raises when the queue overflowed can be seen.
             */
            nfds = gui_input_get_pollfds(p->input, fds);
            if (nfds < 0) {
                rc = fail(1, errno, "get_pollfds");
                break;
            }
        } else {
            nfds = 0;
        }

        /* The step. */
        if (!p->step_paused) {
            gui_status_t status = gui_platform_step(p->platform, &delay_ms);

            if (status != GUI_STATUS_OK && status != GUI_STATUS_EAGAIN) {
                if (p->scenario == SC_OWNER && p->owner_released &&
                    (status == GUI_STATUS_LOST_OWNERSHIP || errno == EACCES)) {
                    /* The ownership loss the owner scenario waits for. */
                    p->owner_loss_seen = true;
                    break;
                }
                rc = fail(1, errno, "platform_step");
                break;
            }
        }

        /* Wait for the next round: the control file must be polled at
         * least every 10 ms, so the wait is never longer than that. */
        timeout_ms = (int)CTL_POLL_MS;
        if (delay_ms < (uint32_t)timeout_ms) {
            timeout_ms = (int)delay_ms;
        }
        if (nfds > 0) {
            int pr = poll(fds, (nfds_t)nfds, timeout_ms);
            if (pr < 0 && errno != EINTR) {
                rc = fail(1, errno, "poll");
                break;
            }
            /*
             * The overflow scenario: the POLLERR the kernel raises when the
             * ACTIVE keyboard queue overflowed is the ONLY way to see the
             * latched reason.  The design mandates exactly ONE non-blocking
             * read for it (the dispatch performs it), and that read fails
             * with the latched errno -- EOVERFLOW -- without consuming the
             * queue.  This is the error observation, not a consumption:
             * the dispatch stops at the latched error and delivers nothing.
             */
            if (p->scenario == SC_OVERFLOW && !p->overflow_seen) {
                bool poll_err = false;
                int i;

                for (i = 0; i < nfds; i++) {
                    if (fds[i].revents & POLLERR) {
                        poll_err = true;
                    }
                }
                if (poll_err) {
                    p->overflow_seen = true;
                    if (gui_input_dispatch(p->input, fds, (unsigned)nfds) != 0) {
                        /* errno is the latched reason: EOVERFLOW. */
                        rc = fail(1, errno, "input_dispatch");
                        break;
                    }
                    /* The dispatch did NOT fail: nothing overflowed. */
                    rc = fail(1, ENOSPC, "overflow_not_observed");
                    break;
                }
            }
        } else {
            poll(NULL, 0, timeout_ms);
        }

        /* The raw device pump (after the wait). */
        if (!p->dispatch_paused && nfds > 0) {
            if (gui_input_dispatch(p->input, fds, (unsigned)nfds) != 0) {
                rc = fail(1, errno, "input_dispatch");
                break;
            }
        }

        /* The scenario state machines. */
        if (p->scenario == SC_INTERACTION || p->scenario == SC_ISOLATION
            || p->scenario == SC_CRASH || p->scenario == SC_FATAL) {
            /*
             * The fatal scenario reports its state like the other ones:
             * the runner has to see that the guest holds the input while
             * it is killed, and that is only visible in the STATE stream.
             * The trigger record below stays DISTINGUISHABLE: it carries
             * the "fatal_trigger=1" field.
             */
            if (!emit_state_if_changed(p)) {
                rc = fail(1, EOVERFLOW, "state_record"); goto cleanup;
            }
        }

        if (p->scenario == SC_FATAL && p->fatal_seen) {
            /* A real PS/2 F12 DOWN arrived: trigger the LVGL assertion. */
            volatile int trigger = 1;
            char fields[PROTO_MAX_EXTRA];

            observe_state(p);
            if (!build_state_fields(p, fields, sizeof(fields), "fatal_trigger=1")) {
                rc = fail(1, EOVERFLOW, "state_fields"); goto cleanup;
            }
            if (!proto_emit(p, "STATE", "%s", fields)) {
                rc = fail(1, EOVERFLOW, "state_record"); goto cleanup;
            }
            snprintf(p->last_state, sizeof(p->last_state), "%s", fields);
            p->state_emitted = true;
            fflush(stdout);
            /*
             * The control file must not outlive the probe, and
             * LV_ASSERT_HANDLER is _exit(1): the assertion never returns to
             * the cleanup block.  Remove the file BEFORE the assertion.
             */
            unlink(p->ctl_path);
            LV_ASSERT_MSG(trigger == 0,
                          "gui-input probe: fatal scenario (real PS/2 F12 DOWN)");
            /* Unreachable: the assertion handler exits the process. */
            rc = fail(1, EFAULT, "fatal_assert_did_not_exit"); goto cleanup;
        }

        if (p->scenario == SC_OWNER && !p->owner_released) {
            /* Wait for a still first frame before the release. */
            if (p->platform->flush_count != p->flush_count_seen) {
                p->flush_count_seen = p->platform->flush_count;
                p->still_frames = 0;
            } else {
                p->still_frames++;
            }
        }

        /* The bound. */
        if (now - p->start_ms >= p->limit_ms) {
            break;
        }
    }

    if (rc == 0) {
        /* The scenario's own assertion. */
        const char *result = "PASS";
        const char *detail = "ok";
        int exit_code = 0;

        observe_state(p);
        if (!emit_state_if_changed(p)) {
            rc = fail(1, EOVERFLOW, "final_state"); goto cleanup;
        }

        if (sigint_seen) {
            /* A normal lease never sees a SIGINT. */
            result = "FAIL";
            detail = "sigint_seen";
            exit_code = 1;
        } else if (p->scenario == SC_INTERACTION || p->scenario == SC_ISOLATION
                   || p->scenario == SC_CRASH) {
            if (!p->ended_by_shortcut) {
                result = "FAIL";
                detail = "no_exit_shortcut";
                exit_code = 1;
            } else if (p->scenario == SC_ISOLATION) {
                detail = "isolation_complete";
            } else if (p->scenario == SC_CRASH) {
                detail = "crash_complete";
            } else {
                detail = "interaction_complete";
            }
        } else if (p->scenario == SC_FATAL) {
            result = "FAIL";
            detail = "no_f12";
            exit_code = 1;
        } else if (p->scenario == SC_OWNER) {
            if (!p->owner_loss_seen) {
                result = "FAIL";
                detail = "no_owner_loss";
                exit_code = 1;
            } else {
                /*
                 * The ownership loss IS the expected detection, so the
                 * record says PASS.  The PROCESS still exits non-zero: it
                 * did not end through its normal path (it lost the view
                 * under its feet), and the runner asserts both.
                 */
                detail = "owner_loss_detected";
                exit_code = 1;
            }
        } else if (p->scenario == SC_OVERFLOW) {
            /*
             * Reaching this point means the scenario ran out of its life
             * WITHOUT the queue overflowing: that is a FAIL, never a
             * PASS.  The overflow itself ends the scenario earlier, with
             * the latched errno (EOVERFLOW) in an ERROR record.
             */
            result = "FAIL";
            detail = p->overflow_seen ? "overflow_not_latched" : "no_overflow";
            exit_code = 1;
        } else if (p->scenario == SC_HOLD) {
            /* The hold only has to survive: no key may end it, and the
             * SIGINT flag must still be clear. */
            detail = p->owner_released ? "hold_released" : "hold_complete";
        } else {
            detail = "ok";
        }

        if (!emit_exit(p, result, exit_code, detail)) {
            rc = fail(1, EOVERFLOW, "exit_record"); goto cleanup;
        }
        rc = exit_code;
    }

    /* ---- cleanup ---------------------------------------------------- */
cleanup:
    if (p->input != NULL) {
        gui_input_destroy(p->input);
        p->input = NULL;
    }
    if (p->platform != NULL) {
        gui_platform_destroy(p->platform);
        p->platform = NULL;
    }
    /* The control file never outlives the probe. */
    unlink(p->ctl_path);

    return rc;
}
