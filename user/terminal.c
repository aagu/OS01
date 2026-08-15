/* terminal.elf — OS01 userspace VT100 terminal emulator
 *
 * Keyboard path: open /dev/tty BEFORE ctty set → CTTY_NONE → phys TTY
 *   kbd IRQ → kbd_tty ring buffer → /dev/tty fd → terminal.elf → PTY master → ash
 *
 * Ash output path: ash → PTY slave → pipe → PTY master fd → terminal.elf → fb
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>     // environ
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <termios.h>
#include "terminal_core.h"

// ── fb_info (must match kernel definition) ──────────────────
struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define FBIOSURRENDER  0x00004601

// ── PSF2 font header (must match kernel font.h) ─────────────
// 8 uint32 fields + glyphs byte + packed = 33 bytes
typedef struct {
    uint32_t magic, version, headersize, flags, numglyph, bytesperglyph, height, width;
    uint8_t glyphs;
} __attribute__((packed)) psf2_t;

// Embedded font data (from objcopy)
// Input file: terminal_font.psf → symbol: _binary_terminal_font_psf_start
extern char _binary_terminal_font_psf_start[];

// ── Terminal state ──────────────────────────────────────────
static uint32_t *fb;
static struct fb_info fb_info;
static psf2_t *font;
static int term_cols, term_rows;
static uint32_t fg = 0xFFFFFFFF, bg = 0x00000000;
static term_core_t core;   // VT100 screen model + parser (terminal_core.c)

// ═══════════════════════════════════════════════════════════
//  Renderer
// ═══════════════════════════════════════════════════════════

static void put_glyph(int col, int row, uint32_t fgc, uint32_t bgc, char c)
{
    if (col < 0 || col >= term_cols || row < 0 || row >= term_rows) return;
    if (c <= 0 || c >= (int)font->numglyph) c = ' ';

    unsigned char *glyph = (unsigned char *)font + font->headersize
        + (unsigned int)c * font->bytesperglyph;

    for (uint32_t y = 0; y < font->height; y++) {
        uint32_t *line = fb + (row * font->height + y) * (fb_info.stride / 4)
                         + col * font->width;
        uint32_t test = 0x100;
        for (uint32_t x = 0; x < font->width; x++) {
            test >>= 1;
            *line++ = (*glyph & test) ? fgc : bgc;
        }
        glyph++;
    }
}

static void flush_screen(void)
{
    term_cell_t (*screen)[TERM_COLS] = term_core_screen(&core);
    for (int r = 0; r < core.rows; r++)
        for (int c = 0; c < core.cols; c++)
            if (term_core_is_dirty(&core, r, c)) {
                uint8_t g = screen[r][c].glyph;
                put_glyph(c, r, fg, bg, g ? (char)g : ' ');
                term_core_clear_dirty(&core, r, c);
            }
}

// ═══════════════════════════════════════════════════════════
//  VT100 output parser
// ═══════════════════════════════════════════════════════════

static void output_char(char c)
{
    if (term_core_input(&core, c))
        flush_screen();
}

static void handle_output(char *buf, int n, int serial_fd)
{
    for (int i = 0; i < n; i++) {
        output_char(buf[i]);
        if (serial_fd >= 0) write(serial_fd, &buf[i], 1);
    }
}

// ═══════════════════════════════════════════════════════════
//  Input handler (dual-mode: cooked / raw)
// ═══════════════════════════════════════════════════════════

static void handle_input(char *buf, int n, int pty_fd, int ash_pid)
{
    // Raw passthrough: ash (FEATURE_EDITING=y) handles all line editing, echo, ^C.
    // We just forward keyboard bytes to the PTY master and let ash do the rest.
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

#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/syscall.h>  // SYS_putchar for early debug

#define ASH_PATH "/bin/busybox"

// Early debug: write a string to serial via kernel putchar syscall
static void dbg(const char *s) {
    while (*s) syscall(SYS_putchar, (uint64_t)*s++, 0, 0);
}

int main(void)
{
    char *ash_argv[] = { "ash", NULL };

    // 1. Open /dev/tty
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) { exec(ASH_PATH, ash_argv, environ); return 1; }

    // 2. Try framebuffer
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) { dup2(tty_fd, 0); close(tty_fd); exec(ASH_PATH, ash_argv, environ); return 1; }

    read(fb_fd, &fb_info, sizeof(fb_info));
    fb = mmap(NULL, fb_info.height * fb_info.stride,
              PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_info.width == 0 || (int64_t)(intptr_t)fb < 0) {
        // No fb — run ash directly on TTY
        if (fb_fd >= 0) close(fb_fd);
        dup2(tty_fd, 0); close(tty_fd);
        exec(ASH_PATH, ash_argv, environ);
        return 1;
    }

    ioctl(fb_fd, FBIOSURRENDER, NULL);
    font = (psf2_t *)_binary_terminal_font_psf_start;
    term_cols = fb_info.width / font->width;
    term_rows = fb_info.height / font->height;
    term_core_init(&core, term_rows, term_cols);

    // 3. Open serial for headless echo
    int serial_fd = open("/dev/serial", O_WRONLY);

    // 4. PTY
    int pty_fd = open("/dev/ptmx", O_RDWR);
    if (pty_fd < 0) { close(tty_fd); close(fb_fd); exec(ASH_PATH, ash_argv, environ); return 1; }
    int slave = open("/dev/pts0", O_RDWR);
    if (slave < 0) { close(pty_fd); close(tty_fd); close(fb_fd); exec(ASH_PATH, ash_argv, environ); return 1; }

    // 5. Fork ash
    int ash_pid = fork();
    if (ash_pid == 0) {
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
        exec(ASH_PATH, ash_argv, environ);
        exit(1);
    }
    close(slave);

    // 6. Main loop — mirror fb output to serial for headless
    struct pollfd fds[2] = {{.fd = tty_fd, .events = POLLIN}, {.fd = pty_fd, .events = POLLIN}};
    char buf[256];
    while (1) {
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & POLLIN) {
            int n = read(tty_fd, buf, sizeof(buf));
            if (n > 0) handle_input(buf, n, pty_fd, ash_pid);
        }
        if (fds[1].revents & POLLIN) {
            int n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) {
                handle_output(buf, n, serial_fd);
            } else if (n == 0) break;
            else if (errno == EINTR) continue; else break;
        }
    }

    waitpid(ash_pid, NULL, 0);
    munmap(fb, fb_info.height * fb_info.stride);
    close(pty_fd); close(tty_fd); close(fb_fd);
    return 0;
}
