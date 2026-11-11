#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"

#ifndef OS01_HOST_TEST
#include <gfx.h>
#include "doom_runtime.h"
#include "doom_video.h"
#endif

/* Forward declarations for Doom key bindings */
extern int key_fire;
extern int key_use;
extern int key_speed;
extern int key_strafe;
extern int key_right;
extern int key_left;
extern int key_up;
extern int key_down;

static char s_nonce[64] = "default";
static char s_action[64] = "";
static int s_seq = 1;
static int s_first_frame_done = 0;
static uint32_t s_frame_count = 0;
static uint32_t s_eagain_count = 0;
static uint32_t s_err_count = 0;
static int s_serial_fd = -1;
static int s_prev_had_cmd = 0;

void doom_diag_set_output_fd(int fd)
{
    s_serial_fd = fd;
}

void doom_diag_init(const char *nonce)
{
    if (nonce && nonce[0]) {
        strncpy(s_nonce, nonce, sizeof(s_nonce) - 1);
        s_nonce[sizeof(s_nonce) - 1] = '\0';
    }
    s_seq = 1;
    s_first_frame_done = 0;
    s_frame_count = 0;
    s_eagain_count = 0;
    s_err_count = 0;
    s_prev_had_cmd = 0;
    s_action[0] = '\0';
}

void doom_diag_set_action(const char *action)
{
    if (action) {
        strncpy(s_action, action, sizeof(s_action) - 1);
        s_action[sizeof(s_action) - 1] = '\0';
    } else {
        s_action[0] = '\0';
    }
}

const char *doom_diag_get_action(void)
{
    return s_action;
}

static int get_serial_fd(void)
{
    if (s_serial_fd < 0) {
        s_serial_fd = open("/dev/serial", O_WRONLY | O_NOCTTY);
    }
    return s_serial_fd >= 0 ? s_serial_fd : STDERR_FILENO;
}

void doom_diag_emit(const char *type, const char *fmt, ...)
{
    char buf[1024];
    int prefix_len = snprintf(buf, sizeof(buf),
                              "[doom-diag] v=1 nonce=%s pid=%d seq=%d type=%s",
                              s_nonce, getpid(), s_seq++, type ? type : "unknown");
    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(buf) - 2) {
        return;
    }

    if (fmt && fmt[0]) {
        buf[prefix_len++] = ' ';
        va_list ap;
        va_start(ap, fmt);
        int rest_len = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len - 1, fmt, ap);
        va_end(ap);
        if (rest_len > 0) {
            prefix_len += rest_len;
        }
    }

    if (prefix_len < (int)sizeof(buf) - 1) {
        buf[prefix_len++] = '\n';
        buf[prefix_len] = '\0';
    } else {
        buf[sizeof(buf) - 2] = '\n';
        buf[sizeof(buf) - 1] = '\0';
        prefix_len = (int)sizeof(buf) - 1;
    }

    int fd = get_serial_fd();
    if (fd >= 0) {
        ssize_t written = 0;
        while (written < prefix_len) {
            ssize_t n = write(fd, buf + written, (size_t)(prefix_len - written));
            if (n <= 0) break;
            written += n;
        }
    }
}

void doom_diag_first_frame(void)
{
    if (!s_first_frame_done) {
        s_first_frame_done = 1;
        s_frame_count++;
        doom_diag_emit("ready", "frames=%u", s_frame_count);
    }
}

int doom_diag_first_frame_done(void)
{
    return s_first_frame_done;
}

void doom_diag_present_stat(int prc, int err, uint64_t duration_us)
{
    (void)duration_us;
    if (prc == 0) {
        s_frame_count++;
        if (!s_first_frame_done) {
            s_first_frame_done = 1;
            doom_diag_emit("ready", "frames=%u", s_frame_count);
        }
    } else if (err == EAGAIN) {
        s_eagain_count++;
    } else {
        s_err_count++;
    }
}

void doom_diag_tic(int gametic, int leveltime, int gamestate, int map,
                   int x, int y, int angle,
                   int forwardmove, int sidemove, int angleturn, int buttons)
{
    int has_cmd = (buttons != 0 || forwardmove != 0 || sidemove != 0 || angleturn != 0);
    bool should_emit = has_cmd || s_prev_had_cmd || (gametic <= 250) ||
                       (gametic % 35 == 0 && gametic <= 1500);

    if (should_emit) {
        doom_diag_emit("tic",
                       "gametic=%d leveltime=%d gamestate=%d map=%d x=%d y=%d angle=%u fwd=%d side=%d turn=%d buttons=%d",
                       gametic, leveltime, gamestate, map, x, y, (unsigned)angle,
                       forwardmove, sidemove, angleturn, buttons);
    }
    s_prev_had_cmd = has_cmd;
}

void doom_diag_save(int slot, const char *desc, int map, int x, int y, int angle, int hp)
{
    doom_diag_emit("save",
                   "slot=%d desc=%s map=%d x=%d y=%d angle=%u hp=%d",
                   slot, desc && desc[0] ? desc : "none", map, x, y, (unsigned)angle, hp);
}

void doom_diag_load(int slot, int map, int x, int y, int angle, int hp)
{
    doom_diag_emit("load",
                   "slot=%d map=%d x=%d y=%d angle=%u hp=%d",
                   slot, map, x, y, (unsigned)angle, hp);
}

void doom_diag_bindings(void)
{
    doom_diag_emit("bindings",
                   "fire=%d use=%d speed=%d strafe=%d right=%d left=%d up=%d down=%d",
                   key_fire, key_use, key_speed, key_strafe,
                   key_right, key_left, key_up, key_down);
}

void doom_diag_summary(void)
{
    doom_diag_emit("summary",
                   "frames=%u eagain=%u errs=%u",
                   s_frame_count, s_eagain_count, s_err_count);
}

void doom_diag_self_release(void)
{
#ifndef OS01_HOST_TEST
    doom_runtime_t *rt = doom_runtime_get_active();
    if (rt && rt->video.handle) {
        gfx_release(rt->video.handle);
    }
#endif
}
