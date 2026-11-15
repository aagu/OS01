#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <gfx.h>
#include <uapi/fb.h>
#include <uapi/mouse.h>

static volatile int g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int get_screen_dimensions(uint32_t *out_w, uint32_t *out_h)
{
    int fd = open("/dev/fb", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "usb-hid-probe: open /dev/fb failed (errno=%d)\n", errno);
        return -1;
    }
    struct fb_info info;
    ssize_t r = read(fd, &info, sizeof(info));
    close(fd);
    if (r != (ssize_t)sizeof(info) || info.width == 0 || info.height == 0) {
        fprintf(stderr, "usb-hid-probe: invalid /dev/fb metadata\n");
        return -1;
    }
    *out_w = info.width;
    *out_h = info.height;
    return 0;
}

static size_t drain_stdin_nonblock(void)
{
    uint8_t buf[128];
    size_t total = 0;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0 || !(pfd.revents & POLLIN))
            break;
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        break;
    }
    return total;
}

static int run_input_mode(const char *nonce)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "usb-hid-probe: sigaction failed (errno=%d)\n", errno);
        return 1;
    }

    int kbd_fd = open("/dev/keyboard", O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0) {
        fprintf(stderr, "usb-hid-probe: open /dev/keyboard failed (errno=%d)\n", errno);
        return 1;
    }
    int mouse_fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
    if (mouse_fd < 0) {
        fprintf(stderr, "usb-hid-probe: open /dev/mouse failed (errno=%d)\n", errno);
        close(kbd_fd);
        return 1;
    }

    uint32_t w = 0, h = 0;
    if (get_screen_dimensions(&w, &h) != 0) {
        close(mouse_fd);
        close(kbd_fd);
        return 1;
    }

    gfx_handle_t *gh = gfx_open(0, 0, w, h);
    if (!gh) {
        fprintf(stderr, "usb-hid-probe: gfx_open failed (errno=%d)\n", errno);
        close(mouse_fd);
        close(kbd_fd);
        return 1;
    }
    if (gfx_acquire(gh) != 0) {
        fprintf(stderr, "usb-hid-probe: gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh);
        close(mouse_fd);
        close(kbd_fd);
        return 1;
    }
    gfx_fill_rect(gh, 0, 0, w, h, 0x00102030);
    gfx_present(gh);

    gfx_input_state_t in_state;
    memset(&in_state, 0, sizeof(in_state));
    if (gfx_bind_input(gh, kbd_fd, mouse_fd, &in_state) != 0) {
        fprintf(stderr, "usb-hid-probe: gfx_bind_input failed (errno=%d)\n", errno);
        gfx_release(gh);
        gfx_close(gh);
        close(mouse_fd);
        close(kbd_fd);
        return 1;
    }

    int saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (saved_stdin_flags < 0 ||
        fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags | O_NONBLOCK) != 0) {
        fprintf(stderr, "usb-hid-probe: fcntl stdin failed (errno=%d)\n", errno);
        gfx_unbind_input(gh);
        gfx_release(gh);
        gfx_close(gh);
        close(mouse_fd);
        close(kbd_fd);
        return 1;
    }

    /* Drain any pre-existing bytes before announcing READY. */
    (void)drain_stdin_nonblock();

    printf("USB_HID_INPUT_READY nonce=%s gen=%llu\n",
           nonce, (unsigned long long)in_state.generation);
    fflush(stdout);

    bool e0_pending = false;
    size_t leaked_bytes = 0;

    while (!g_stop) {
        struct pollfd pfds[3];
        pfds[0].fd = kbd_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = mouse_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        pfds[2].fd = STDIN_FILENO;
        pfds[2].events = POLLIN;
        pfds[2].revents = 0;

        int pr = poll(pfds, 3, 50);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        uint8_t kbuf[64];
        for (;;) {
            ssize_t n = read(kbd_fd, kbuf, sizeof(kbuf));
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; i++) {
                uint8_t b = kbuf[i];
                if (!e0_pending && b == 0xE0) {
                    e0_pending = true;
                } else if (e0_pending) {
                    e0_pending = false;
                    printf("KEY_FRAME nonce=%s hex=e0%02x\n", nonce, (unsigned)b);
                } else {
                    printf("KEY_FRAME nonce=%s hex=%02x\n", nonce, (unsigned)b);
                    if (b == 0x81)
                        g_stop = 1;
                }
            }
        }

        mouse_event_t mev[16];
        for (;;) {
            ssize_t n = read(mouse_fd, mev, sizeof(mev));
            if (n <= 0)
                break;
            size_t count = (size_t)n / sizeof(mouse_event_t);
            for (size_t i = 0; i < count; i++) {
                printf("MOUSE_EVENT nonce=%s btn=%u dx=%d dy=%d wheel=%d\n",
                       nonce,
                       (unsigned)mev[i].buttons,
                       (int)mev[i].dx,
                       (int)mev[i].dy,
                       (int)mev[i].wheel);
            }
        }

        if (pfds[2].revents & POLLIN) {
            leaked_bytes += drain_stdin_nonblock();
        }
        fflush(stdout);
    }

    leaked_bytes += drain_stdin_nonblock();
    printf("PTY_ISOLATION nonce=%s leaked_bytes=%llu\n",
           nonce, (unsigned long long)leaked_bytes);

    (void)fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags);
    gfx_unbind_input(gh);
    gfx_release(gh);
    gfx_close(gh);
    close(mouse_fd);
    close(kbd_fd);

    printf("USB_HID_INPUT_DONE nonce=%s leaked_bytes=%llu\n",
           nonce, (unsigned long long)leaked_bytes);
    fflush(stdout);
    return leaked_bytes == 0 ? 0 : 1;
}

static int run_io_mode(const char *nonce, int count)
{
    char path[128];
    snprintf(path, sizeof(path), "/usb-hid-io-%.32s.dat", nonce);

    uint8_t wbuf[4096];
    uint8_t rbuf[4096];

    printf("IO_BEGIN nonce=%s\n", nonce);
    fflush(stdout);

    for (int iter = 0; iter < count; iter++) {
        size_t hdr_len = (size_t)snprintf((char *)wbuf, sizeof(wbuf), "%s:%d:", nonce, iter);
        for (size_t i = hdr_len; i < sizeof(wbuf); i++) {
            wbuf[i] = (uint8_t)(((i * 31u) + (unsigned)iter) & 0xFFu);
        }

        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "IO_ERROR nonce=%s iter=%d open_w errno=%d\n", nonce, iter, errno);
            return 1;
        }
        size_t written = 0;
        while (written < sizeof(wbuf)) {
            ssize_t n = write(fd, wbuf + written, sizeof(wbuf) - written);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "IO_ERROR nonce=%s iter=%d write errno=%d\n", nonce, iter, errno);
                close(fd);
                return 1;
            }
            if (n == 0) {
                fprintf(stderr, "IO_ERROR nonce=%s iter=%d short_write\n", nonce, iter);
                close(fd);
                return 1;
            }
            written += (size_t)n;
        }
        if (close(fd) != 0) {
            fprintf(stderr, "IO_ERROR nonce=%s iter=%d close errno=%d\n", nonce, iter, errno);
            return 1;
        }

        if (syscall(SYS_sync, 0, 0, 0) != 0) {
            fprintf(stderr, "IO_ERROR nonce=%s iter=%d sync errno=%d\n", nonce, iter, errno);
            return 1;
        }

        fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "IO_ERROR nonce=%s iter=%d open_r errno=%d\n", nonce, iter, errno);
            return 1;
        }
        size_t got = 0;
        while (got < sizeof(rbuf)) {
            ssize_t n = read(fd, rbuf + got, sizeof(rbuf) - got);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "IO_ERROR nonce=%s iter=%d read errno=%d\n", nonce, iter, errno);
                close(fd);
                return 1;
            }
            if (n == 0)
                break;
            got += (size_t)n;
        }
        close(fd);

        if (got != sizeof(wbuf) || memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
            fprintf(stderr, "IO_ERROR nonce=%s iter=%d verify_mismatch got=%llu\n",
                    nonce, iter, (unsigned long long)got);
            return 1;
        }

        printf("IO_PROGRESS nonce=%s iter=%d bytes=%llu\n",
               nonce, iter, (unsigned long long)sizeof(wbuf));
        fflush(stdout);

        if (iter + 1 < count) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
            nanosleep(&ts, NULL);
        }
    }

    printf("IO_END nonce=%s iters=%d\n", nonce, count);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argv[2][0] == '\0') {
        fprintf(stderr, "usage: usb-hid-probe <input|io> <nonce> [count]\n");
        return 1;
    }

    if (strcmp(argv[1], "input") == 0) {
        return run_input_mode(argv[2]);
    }
    if (strcmp(argv[1], "io") == 0) {
        int count = 12;
        if (argc >= 4) {
            count = atoi(argv[3]);
            if (count < 1)
                count = 1;
        }
        return run_io_mode(argv[2], count);
    }

    fprintf(stderr, "usb-hid-probe: unknown mode '%s'\n", argv[1]);
    return 1;
}
