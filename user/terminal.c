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
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <termios.h>

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
static int term_col, term_row;
static int term_cols, term_rows;
static uint32_t fg = 0xFFFFFFFF, bg = 0x00000000;
static bool cursor_visible = true;

// ── Input line buffer ───────────────────────────────────────
static char line_buf[256];
static int  line_len;

// ── VT100 output parser state ───────────────────────────────
enum { S_NORMAL, S_ESC, S_CSI_PARAM };
static int csi_state = S_NORMAL;
static int csi_param = 0;
static bool csi_qmark = false;

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

static void fb_scroll(void)
{
    uint8_t *fb_bytes = (uint8_t *)fb;
    uint32_t row_bytes = font->height * fb_info.stride;
    uint32_t total = row_bytes * term_rows;
    memmove(fb_bytes, fb_bytes + row_bytes, total - row_bytes);
    memset(fb_bytes + total - row_bytes, 0, row_bytes);
    term_row = term_rows - 1;
}

// ═══════════════════════════════════════════════════════════
//  VT100 output parser
// ═══════════════════════════════════════════════════════════

static void output_char(char c)
{
    if (csi_state == S_NORMAL && c == '\x1b') {
        csi_state = S_ESC;
        return;
    }
    if (csi_state == S_ESC) {
        if (c == '[') { csi_state = S_CSI_PARAM; csi_param = 0; csi_qmark = false; return; }
        csi_state = S_NORMAL;
        return;
    }
    if (csi_state == S_CSI_PARAM) {
        if (c == '?') { csi_qmark = true; return; }
        if (c >= '0' && c <= '9') { csi_param = csi_param * 10 + (c - '0'); return; }
        // Terminal character
        switch (c) {
        case 'A': term_row -= (csi_param ? csi_param : 1); if (term_row < 0) term_row = 0; break;
        case 'B': term_row += (csi_param ? csi_param : 1); if (term_row >= term_rows) term_row = term_rows - 1; break;
        case 'C': term_col += (csi_param ? csi_param : 1); if (term_col >= term_cols) term_col = term_cols - 1; break;
        case 'D': term_col -= (csi_param ? csi_param : 1); if (term_col < 0) term_col = 0; break;
        case 'K':
            if (csi_param == 0)
                for (int x = term_col; x < term_cols; x++) put_glyph(x, term_row, fg, bg, ' ');
            else if (csi_param == 1)
                for (int x = 0; x <= term_col; x++) put_glyph(x, term_row, fg, bg, ' ');
            else
                for (int x = 0; x < term_cols; x++) put_glyph(x, term_row, fg, bg, ' ');
            break;
        case 'J':
            if (csi_param == 2) {
                for (int r = 0; r < term_rows; r++)
                    for (int x = 0; x < term_cols; x++)
                        put_glyph(x, r, fg, bg, ' ');
                term_row = 0; term_col = 0;
            }
            break;
        case 'H': term_row = 0; term_col = 0; break;
        case 'h': if (csi_qmark && csi_param == 25) cursor_visible = true; break;
        case 'l': if (csi_qmark && csi_param == 25) cursor_visible = false; break;
        }
        csi_state = S_NORMAL;
        return;
    }

    // Normal character
    switch (c) {
    case '\n': term_col = 0; term_row++; break;
    case '\r': term_col = 0; break;
    case '\b': case 0x7F: if (term_col > 0) term_col--; break;
    case '\t': term_col = (term_col + 8) & ~7; break;
    default:
        if ((unsigned char)c >= ' ') {
            put_glyph(term_col, term_row, fg, bg, c);
            term_col++;
        }
        break;
    }

    if (term_col >= term_cols) { term_col = 0; term_row++; }
    if (term_row >= term_rows) fb_scroll();
}

static void handle_output(char *buf, int n)
{
    for (int i = 0; i < n; i++)
        output_char(buf[i]);
}

// ═══════════════════════════════════════════════════════════
//  Input handler (dual-mode: cooked / raw)
// ═══════════════════════════════════════════════════════════

static void handle_input(char *buf, int n, int pty_fd, int ash_pid)
{
    struct termios term;
    bool is_cooked = (ioctl(pty_fd, TCGETS, &term) == 0) && (term.c_lflag & ICANON);

    for (int i = 0; i < n; i++) {
        char c = buf[i];

        if (!is_cooked) {
            write(pty_fd, &c, 1);
            continue;
        }

        // Cooked mode with line editing
        if (c == '\r' || c == '\n') {
            line_buf[line_len++] = '\n';
            write(pty_fd, line_buf, line_len);
            line_len = 0;
            output_char('\r'); output_char('\n');
        } else if (c == '\x7f' || c == '\b') {
            if (line_len > 0) {
                line_len--;
                if (term_col > 0) term_col--;
                put_glyph(term_col, term_row, fg, bg, ' ');
            }
        } else if (c == '\x03') {
            if (term.c_lflag & ISIG)
                kill(ash_pid, SIGINT);
        } else if (c >= ' ') {
            if (line_len < (int)sizeof(line_buf) - 1) {
                line_buf[line_len++] = c;
                output_char(c);
            }
        }
        // \x04 (Ctrl-D) on empty line: ignored in V1 (ash exits on EOF)
    }
}

// ═══════════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════════

int main(void)
{
    // 1. Open physical TTY BEFORE ctty is set (CTTY_NONE → phys TTY)
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) { write(2, "terminal: /dev/tty\n", 19); return 1; }

    // 2. Open framebuffer
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) { write(2, "terminal: /dev/fb\n", 18); return 1; }

    read(fb_fd, &fb_info, sizeof(fb_info));
    fb = mmap(NULL, fb_info.height * fb_info.stride,
              PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((int64_t)(intptr_t)fb < 0) {
        write(2, "terminal: mmap fb failed\n", 25); return 1;
    }

    ioctl(fb_fd, FBIOSURRENDER, NULL);

    // Setup PSF2 font
    font = (psf2_t *)_binary_terminal_font_psf_start;
    term_cols = fb_info.width / font->width;
    term_rows = fb_info.height / font->height;
    term_col = 0; term_row = 0;

    // 3. Open PTY master
    int pty_fd = open("/dev/ptmx", O_RDWR);
    if (pty_fd < 0) { write(2, "terminal: /dev/ptmx\n", 19); return 1; }

    // 4. Open slave → sets ctty=PTY for this session + fork children
    int slave = open("/dev/pts0", O_RDWR);
    if (slave < 0) { write(2, "terminal: /dev/pts0\n", 19); return 1; }

    // 5. Fork ash (child inherits ctty=PTY)
    int ash_pid = fork();
    if (ash_pid == 0) {
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
        char *argv[] = { "/bin/ash", NULL };
        exec("/bin/ash", argv, NULL);
        exit(1);
    }
    close(slave);

    // 6. Main loop
    struct pollfd fds[2] = {{.fd = tty_fd, .events = POLLIN}, {.fd = pty_fd, .events = POLLIN}};
    char buf[256];
    int exit_code = 0;

    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            int n = read(tty_fd, buf, sizeof(buf));
            if (n > 0) handle_input(buf, n, pty_fd, ash_pid);
        }
        if (fds[1].revents & POLLIN) {
            int n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) handle_output(buf, n);
            else if (n == 0) break;
            else if (errno == EINTR) continue;
            else break;
        }
    }

    // Wait for ash to exit (reap zombie)
    waitpid(ash_pid, &exit_code, 0);

    // Cleanup
    munmap(fb, fb_info.height * fb_info.stride);
    close(pty_fd); close(tty_fd); close(fb_fd);
    return exit_code;
}
