/* Interactive /dev/mouse ABI and motion probe. */
#include <uapi/mouse.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/mouse", O_RDONLY);
    if (fd < 0) {
        perror("mousetest: open /dev/mouse");
        return 1;
    }
    char short_buffer[sizeof(mouse_event_t) - 1];
    errno = 0;
    if (read(fd, short_buffer, sizeof(short_buffer)) != -1 || errno != EINVAL) {
        fprintf(stderr, "mousetest: short read did not return EINVAL\n");
        close(fd);
        return 1;
    }
    puts("mousetest: short read EINVAL: PASS");

    mouse_event_t event;
    for (;;) {
        ssize_t n = read(fd, &event, sizeof(event));
        if (n == (ssize_t)sizeof(event))
            continue;
        if (n == -1 && errno == EAGAIN)
            break;
        fprintf(stderr, "mousetest: drain read failed (%ld, errno=%d)\n",
                (long)n, errno);
        close(fd);
        return 1;
    }
    puts("mousetest: empty read EAGAIN: PASS");

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 0);
    if (ready != 0) {
        fprintf(stderr, "mousetest: empty poll expected 0, got %d (revents=%x)\n",
                ready, pfd.revents);
        close(fd);
        return 1;
    }
    puts("mousetest: empty poll: PASS");
    puts("Move mouse, click left/right/middle, and scroll; Ctrl-C to exit.");

    for (;;) {
        pfd.revents = 0;
        ready = poll(&pfd, 1, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("mousetest: poll");
            break;
        }
        if (!(pfd.revents & POLLIN)) {
            fprintf(stderr, "mousetest: unexpected poll events %x\n", pfd.revents);
            break;
        }
        ssize_t n = read(fd, &event, sizeof(event));
        if (n == -1 && errno == EAGAIN)
            continue; /* Another reader may have consumed the event. */
        if (n != (ssize_t)sizeof(event)) {
            fprintf(stderr, "mousetest: event read failed (%ld, errno=%d)\n",
                    (long)n, errno);
            break;
        }
        printf("buttons=%u [L=%u R=%u M=%u] dx=%d dy=%d wheel=%d\n",
               (unsigned)event.buttons,
               (unsigned)(!!(event.buttons & 1)),
               (unsigned)(!!(event.buttons & 2)),
               (unsigned)(!!(event.buttons & 4)),
               (int)event.dx, (int)event.dy, (int)event.wheel);
        fflush(stdout);
    }
    close(fd);
    return 1;
}
