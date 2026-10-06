/* terminal.elf — OS01 userspace VT100 terminal emulator (libgfx backend)
 *
 * Keyboard path: open /dev/tty BEFORE ctty set → CTTY_NONE → phys TTY
 *   kbd IRQ → kbd_tty ring buffer → /dev/tty fd → terminal.elf → PTY master → ash
 *
 * Ash output path: ash → PTY slave → pipe → PTY master fd → terminal.elf → libgfx
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>     // environ
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>   // struct winsize (TIOCSWINSZ target)
#include <termios.h>
#include <time.h>
#include <gfx.h>
#include "terminal_core.h"
#include "terminal_render.h"
#include "terminal_display.h"
#include <uapi/fb.h>
#include <sys/wait.h>

#define ASH_PATH "/bin/busybox"
#define FRAME_INTERVAL_MS 33
#define MAX_PRESENT_FAILURES 5

// Embedded font data (from objcopy)
extern char _binary_terminal_font_psf_start[];
extern char _binary_terminal_font_psf_end[];

// ── Terminal state ──────────────────────────────────────────
// terminal.c owns: fb_fd (parent lifetime), the PTY, ash, the core storage and
// the final cleanup.  terminal_display.c only drives the gfx view/renderer.
static struct fb_state fb_st;
static const psf2_t *font;
static uint32_t fg = 0xFFFFFFFF, bg = 0x00000000;
static term_core_t core;
static terminal_display_t disp;

// ═══════════════════════════════════════════════════════════
//  Input handler (dual-mode: cooked / raw)
// ═══════════════════════════════════════════════════════════

static void handle_input(char *buf, int n, int pty_fd, int ash_pid)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\x03') {
            // ^C → send SIGINT directly (ash handles it even in raw mode)
            kill(ash_pid, SIGINT);
        }
        write(pty_fd, &c, 1);
    }
}

// ═══════════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════════

static void serial_msg(int serial_fd, const char *msg)
{
    if (serial_fd >= 0) write(serial_fd, msg, strlen(msg));
}

// Deliver the current view geometry to the PTY master (4 winsize fields).
// Returns 0 on success (pending cleared) or -1 (still pending, caller retries).
static int send_winsize(int pty_fd)
{
    struct winsize ws;
    unsigned short r, c, x, y;
    if (!terminal_display_winsize(&disp, &r, &c, &x, &y)) return 0;
    ws.ws_row = r; ws.ws_col = c; ws.ws_xpixel = x; ws.ws_ypixel = y;
    if (ioctl(pty_fd, TIOCSWINSZ, &ws) < 0) return -1;
    disp.pending_winsize = false;
    return 0;
}

int main(void)
{
    char *ash_argv[] = { "ash", NULL };
    signal(SIGINT, SIG_IGN);

    int serial_fd = open("/dev/serial", O_WRONLY);

    // 1. Open /dev/tty
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) {
        serial_msg(serial_fd, "[terminal] ERROR: cannot open /dev/tty\r\n");
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 2. Validate embedded PSF2 font
    size_t font_size = (size_t)(_binary_terminal_font_psf_end - _binary_terminal_font_psf_start);
    if (!term_font_validate(_binary_terminal_font_psf_start, font_size, &font)) {
        serial_msg(serial_fd, "[terminal] ERROR: invalid embedded PSF2 font\r\n");
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 3. Query framebuffer state (geometry + mode generation).  fb_fd stays
    //    open for the whole terminal lifetime: it is used to detect mode
    //    switches; the ash child closes its copy.
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) {
        serial_msg(serial_fd, "[terminal] ERROR: cannot open /dev/fb\r\n");
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    if (ioctl(fb_fd, FBIOGET_STATE, &fb_st) < 0 ||
        fb_st.info.width == 0 || fb_st.info.height == 0) {
        serial_msg(serial_fd, "[terminal] ERROR: invalid /dev/fb state\r\n");
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    int term_cols = (int)(fb_st.info.width / font->width);
    int term_rows = (int)(fb_st.info.height / font->height);
    if (term_cols <= 0 || term_rows <= 0) {
        serial_msg(serial_fd, "[terminal] ERROR: non-positive terminal geometry\r\n");
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 4. Open 2D graphics view (/dev/gfx0)
    gfx_handle_t *gfx = gfx_open(0, 0, fb_st.info.width, fb_st.info.height);
    if (!gfx) {
        serial_msg(serial_fd, "[terminal] ERROR: gfx_open failed\r\n");
        close(fb_fd); // Do NOT surrender kernel console
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 5. Initialize terminal core and verify memory buffers
    term_core_init(&core, term_rows, term_cols);
    if (!core.main_buf || !core.alt_buf || !core.dirty) {
        serial_msg(serial_fd, "[terminal] ERROR: buffer allocation failed\r\n");
        term_core_free(&core);
        gfx_close(gfx);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // From here on the view is owned by `disp` (closed once, via
    // terminal_display_close()).
    terminal_display_init(&disp, gfx, &core, font, &fb_st.info,
                          fb_st.generation, fg, bg);
    term_render_clear(&disp.render);
    term_core_mark_all_dirty(&core);
    term_render_flush(&disp.render);

    // 6. Verify initial gfx_present succeeds before taking over console
    if (gfx_present(disp.gfx) != 0) {
        serial_msg(serial_fd, "[terminal] ERROR: initial gfx_present failed\r\n");
        term_core_free(&core);
        terminal_display_close(&disp);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 7. Surrender kernel console and verify
    if (ioctl(fb_fd, FBIOSURRENDER, NULL) < 0) {
        serial_msg(serial_fd, "[terminal] ERROR: FBIOSURRENDER ioctl failed\r\n");
        term_core_free(&core);
        terminal_display_close(&disp);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 8. Post-surrender present: close any brief race window with kernel console
    //    (a mode switch exactly here is recovered by the loop's refresh).
    if (gfx_present(disp.gfx) != 0 && errno != EAGAIN && errno != ESTALE) {
        serial_msg(serial_fd, "[terminal] ERROR: post-surrender gfx_present failed\r\n");
        term_core_free(&core);
        terminal_display_close(&disp);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 9. Allocate PTY
    int pty_fd = open("/dev/ptmx", O_RDWR);
    if (pty_fd < 0) {
        serial_msg(serial_fd, "[terminal] ERROR: open /dev/ptmx failed\r\n");
        term_core_free(&core); terminal_display_close(&disp); close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }
    int slave = open("/dev/pts0", O_RDWR);
    if (slave < 0) {
        serial_msg(serial_fd, "[terminal] ERROR: open /dev/pts0 failed\r\n");
        close(pty_fd); term_core_free(&core); terminal_display_close(&disp);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 10. Initial window size via the PTY master (failure stays pending and is
    //     retried by the event loop; it never rebuilds the graphics).
    disp.pending_winsize = true;
    uint64_t ws_retry_ms = 0;
    (void)send_winsize(pty_fd);

    char *buf = (char *)malloc(2048);

    // 11. Fork ash
    int ash_pid = buf ? fork() : -1;
    if (ash_pid == 0) {
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
        exec(ASH_PATH, ash_argv, environ);
        exit(1);
    }
    close(slave);

    bool done = false;       // skip the loop on early ash failure
    bool fatal_exit = false;
    if (ash_pid < 0) {
        serial_msg(serial_fd, "[terminal] ERROR: fork failed\r\n");
        done = true;
    } else {
        // Make ash's process group the PTY foreground group (master side
        // only; the physical TTY / its group is not touched).
        int pgid = getpgid(ash_pid);
        if (pgid < 0) {
            // ash may already be gone (fast exit): reap it and fall to cleanup.
            if (waitpid(ash_pid, NULL, WNOHANG) == ash_pid) {
                serial_msg(serial_fd, "[terminal] ERROR: ash exited immediately\r\n");
                ash_pid = -1;
                done = true;
            }
        } else if (ioctl(pty_fd, TIOCSPGRP, &pgid) < 0) {
            serial_msg(serial_fd, "[terminal] WARN: PTY TIOCSPGRP failed\r\n");
        }
    }

    // 12. Main event loop: 30 FPS throttle; poll never blocks longer than
    //     250ms so display recovery / winsize retry always make progress.
    struct pollfd fds[2] = {
        {.fd = tty_fd, .events = POLLIN},
        {.fd = pty_fd, .events = POLLIN}
    };
    uint64_t last_present_ms = 0;
    int present_failures = 0;
    bool dirty_pending = false;
    uint64_t last_cmd_submit_ms = 0;
    bool serial_only_logged = false;

#define CMD_HOLD_MS        500

    while (!done) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

        // 0. Display mode recovery (checked every wakeup, independent of
        //    CMD_HOLD and of dirty state).
        uint64_t gen_before = disp.generation;
        int rr = terminal_display_refresh(&disp, fb_fd, now_ms);
        if (rr < 0 && !serial_only_logged) {
            serial_msg(serial_fd, "\r\n[terminal] graphics unavailable, serial only\r\n");
            serial_only_logged = true;
        }
        if (disp.generation != gen_before) {
            last_present_ms = now_ms;       // redraw already presented
            dirty_pending = false;
            present_failures = 0;
        }
        if (disp.pending_winsize && now_ms >= ws_retry_ms) {
            if (send_winsize(pty_fd) != 0) ws_retry_ms = now_ms + TERMINAL_RETRY_MS;
        }

        uint64_t elapsed = now_ms - last_present_ms;
        bool hold_cmd = (now_ms - last_cmd_submit_ms < CMD_HOLD_MS);

        // 1. Present immediately if interval elapsed and not holding for command startup
        if (dirty_pending && elapsed >= FRAME_INTERVAL_MS && !hold_cmd &&
            !(disp.retry_deadline_ms && now_ms < disp.retry_deadline_ms)) {
            if (terminal_display_present(&disp, now_ms) == 0) {
                last_present_ms = now_ms;
                dirty_pending = false;
                present_failures = 0;
            } else {
                int e = errno;
                last_present_ms = now_ms;
                // Mode-related failures are recovered via refresh and never
                // count toward the fatal limit.
                if (e != EAGAIN && e != ESTALE && e != EBUSY &&
                    ++present_failures >= MAX_PRESENT_FAILURES) {
                    serial_msg(serial_fd, "\r\n[terminal] FATAL: graphics present failed repeatedly, aborting\r\n");
                    fatal_exit = true;
                    sleep(2);
                    break;
                }
            }
        }

        // 2. Compute poll timeout
        int base = -1;
        if (dirty_pending) {
            uint64_t cur_elapsed = now_ms - last_present_ms;
            int frame_wait = (cur_elapsed < FRAME_INTERVAL_MS) ?
                             (int)(FRAME_INTERVAL_MS - cur_elapsed) : 0;
            int cmd_wait = (now_ms - last_cmd_submit_ms < CMD_HOLD_MS) ?
                           (int)(CMD_HOLD_MS - (now_ms - last_cmd_submit_ms)) : 0;
            base = (frame_wait > cmd_wait) ? frame_wait : cmd_wait;
        }
        int timeout_ms = terminal_display_poll_timeout(&disp, base, now_ms);

        int pr = poll(fds, 2, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 3. Process keyboard input (forwarded even when graphics are down)
        if (fds[0].revents & POLLIN) {
            int n = read(tty_fd, buf, 2048);
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    if (buf[i] == '\n' || buf[i] == '\r') {
                        last_cmd_submit_ms = now_ms;
                        break;
                    }
                }
                handle_input(buf, n, pty_fd, ash_pid);
            }
        }

        // 4. Process PTY master output (eager drain collapses burst writes)
        if (fds[1].revents & POLLIN) {
            int drain_count = 0;
            const int MAX_DRAIN_CHUNKS = 32; // Drain up to 64 KB per frame to prevent starvation
            bool shell_exited = false;

            while (drain_count < MAX_DRAIN_CHUNKS) {
                int n = read(pty_fd, buf, 2048);
                if (n > 0) {
                    for (int i = 0; i < n; i++) {
                        if (term_core_input(&core, buf[i])) dirty_pending = true;
                    }
                    if (serial_fd >= 0) write(serial_fd, buf, (size_t)n);
                    drain_count++;
                } else if (n == 0) {
                    // Shell died
                    shell_exited = true;
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    shell_exited = true;
                    break;
                }

                // Check if more data is immediately waiting without blocking
                struct pollfd pfd = { .fd = pty_fd, .events = POLLIN };
                int pr_pty = poll(&pfd, 1, 0);
                if (pr_pty <= 0 || !(pfd.revents & POLLIN)) {
                    break;
                }
            }

            if (term_render_cursor_update(&disp.render)) dirty_pending = true;
            if (shell_exited) break;
        }
    }

    // 13. Teardown (the view is closed exactly once, via terminal_display_close)
    if (!fatal_exit && !disp.serial_only && disp.gfx) {
        term_render_clear(&disp.render);
        gfx_present(disp.gfx);
    }
    if (ash_pid > 0) waitpid(ash_pid, NULL, 0);
    term_core_free(&core);
    terminal_display_close(&disp);
    close(fb_fd);
    close(pty_fd); close(tty_fd);
    if (serial_fd >= 0) close(serial_fd);
    free(buf);
    return 0;
}
