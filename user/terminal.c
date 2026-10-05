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

// ── fb_info (must match kernel definition) ──────────────────
struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define FBIOSURRENDER  0x00004601

#define ASH_PATH "/bin/busybox"
#define FRAME_INTERVAL_MS 33
#define MAX_PRESENT_FAILURES 5

// Embedded font data (from objcopy)
extern char _binary_terminal_font_psf_start[];
extern char _binary_terminal_font_psf_end[];

// ── Terminal state ──────────────────────────────────────────
static gfx_handle_t *gfx;
static struct fb_info fb_info;
static const psf2_t *font;
static int term_cols, term_rows;
static uint32_t fg = 0xFFFFFFFF, bg = 0x00000000;
static term_core_t core;
static term_render_t render;

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

int main(void)
{
    char *ash_argv[] = { "ash", NULL };
    signal(SIGINT, SIG_IGN);

    int serial_fd = open("/dev/serial", O_WRONLY);

    // 1. Open /dev/tty
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: cannot open /dev/tty\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 2. Validate embedded PSF2 font
    size_t font_size = (size_t)(_binary_terminal_font_psf_end - _binary_terminal_font_psf_start);
    if (!term_font_validate(_binary_terminal_font_psf_start, font_size, &font)) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: invalid embedded PSF2 font\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 3. Query framebuffer geometry
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: cannot open /dev/fb\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    if (read(fb_fd, &fb_info, sizeof(fb_info)) != (ssize_t)sizeof(fb_info) ||
        fb_info.width == 0 || fb_info.height == 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: invalid /dev/fb metadata\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    term_cols = (int)(fb_info.width / font->width);
    term_rows = (int)(fb_info.height / font->height);
    if (term_cols <= 0 || term_rows <= 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: non-positive terminal geometry\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 4. Open 2D graphics view (/dev/gfx0)
    gfx = gfx_open(0, 0, fb_info.width, fb_info.height);
    if (!gfx) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: gfx_open failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        close(fb_fd); // Do NOT surrender kernel console
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 5. Initialize terminal core and verify memory buffers
    term_core_init(&core, term_rows, term_cols);
    if (!core.main_buf || !core.alt_buf || !core.dirty) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: buffer allocation failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        term_core_free(&core);
        gfx_close(gfx);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    term_render_init(&render, gfx, font, &core, fg, bg);
    term_render_clear(&render);
    term_core_mark_all_dirty(&core);
    term_render_flush(&render);

    // 6. Verify initial gfx_present succeeds before taking over console
    if (gfx_present(gfx) != 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: initial gfx_present failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        term_core_free(&core);
        gfx_close(gfx);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 7. Surrender kernel console and verify
    if (ioctl(fb_fd, FBIOSURRENDER, NULL) < 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: FBIOSURRENDER ioctl failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        term_core_free(&core);
        gfx_close(gfx);
        close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }
    close(fb_fd);

    // 8. Post-surrender present: close any brief race window with kernel console
    if (gfx_present(gfx) != 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: post-surrender gfx_present failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        term_core_free(&core);
        gfx_close(gfx);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    // 9. Allocate PTY
    int pty_fd = open("/dev/ptmx", O_RDWR);
    if (pty_fd < 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: open /dev/ptmx failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        term_core_free(&core); gfx_close(gfx);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }
    int slave = open("/dev/pts0", O_RDWR);
    if (slave < 0) {
        if (serial_fd >= 0) {
            const char msg[] = "[terminal] ERROR: open /dev/pts0 failed\r\n";
            write(serial_fd, msg, sizeof(msg) - 1);
        }
        close(pty_fd); term_core_free(&core); gfx_close(gfx);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    struct winsize ws = {
        .ws_row    = (unsigned short)term_rows,
        .ws_col    = (unsigned short)term_cols,
        .ws_xpixel = (unsigned short)fb_info.width,
        .ws_ypixel = (unsigned short)fb_info.height,
    };
    ioctl(slave, TIOCSWINSZ, &ws);

    // 11. Fork ash
    int ash_pid = fork();
    if (ash_pid == 0) {
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        close(slave); close(pty_fd); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        exit(1);
    }
    close(slave);

    // 12. Main event loop: starvation-free 30 FPS throttle + deep idle sleep
    struct pollfd fds[2] = {
        {.fd = tty_fd, .events = POLLIN},
        {.fd = pty_fd, .events = POLLIN}
    };
    char *buf = (char *)malloc(2048);
    if (!buf) exit(1);
    uint64_t last_present_ms = 0;
    int present_failures = 0;
    bool dirty_pending = false;
    uint64_t last_cmd_submit_ms = 0;
    bool fatal_exit = false;

#define CMD_HOLD_MS        500

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        uint64_t elapsed = now_ms - last_present_ms;
        bool hold_cmd = (now_ms - last_cmd_submit_ms < CMD_HOLD_MS);

        // 1. Present immediately if interval elapsed and not holding for command startup
        if (dirty_pending && elapsed >= FRAME_INTERVAL_MS && !hold_cmd) {
            term_render_flush(&render);
            if (gfx_present(gfx) == 0) {
                last_present_ms = now_ms;
                dirty_pending = false;
                present_failures = 0;
            } else {
                last_present_ms = now_ms;
                present_failures++;
                if (present_failures >= MAX_PRESENT_FAILURES) {
                    if (serial_fd >= 0) {
                        const char msg[] = "\r\n[terminal] FATAL: graphics present failed repeatedly, aborting\r\n";
                        write(serial_fd, msg, sizeof(msg) - 1);
                    }
                    fatal_exit = true;
                    sleep(2);
                    break;
                }
            }
        }

        // 2. Compute poll timeout
        int timeout_ms = -1;
        if (dirty_pending) {
            uint64_t cur_elapsed = now_ms - last_present_ms;
            int frame_wait = (cur_elapsed < FRAME_INTERVAL_MS) ?
                             (int)(FRAME_INTERVAL_MS - cur_elapsed) : 0;
            int cmd_wait = (now_ms - last_cmd_submit_ms < CMD_HOLD_MS) ?
                           (int)(CMD_HOLD_MS - (now_ms - last_cmd_submit_ms)) : 0;
            timeout_ms = (frame_wait > cmd_wait) ? frame_wait : cmd_wait;
        } else {
            timeout_ms = -1; // Idle -> deep sleep
        }

        int pr = poll(fds, 2, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 3. Process keyboard input
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

            if (term_render_cursor_update(&render)) dirty_pending = true;
            if (shell_exited) break;
        }
    }

    // 13. Teardown
    if (!fatal_exit) {
        term_render_clear(&render);
        gfx_present(gfx);
    }
    waitpid(ash_pid, NULL, 0);
    term_core_free(&core);
    gfx_close(gfx);
    close(pty_fd); close(tty_fd);
    if (serial_fd >= 0) close(serial_fd);
    free(buf);
    return 0;
}
