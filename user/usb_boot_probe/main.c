#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] == '\0') {
        fprintf(stderr, "usage: usb-boot-probe <marker>\n");
        return 1;
    }

    const char *marker = argv[1];
    size_t marker_len = strlen(marker);
    if (marker_len > 256) {
        fprintf(stderr, "usb-boot-probe: marker too long\n");
        return 1;
    }

    char payload[260];
    memcpy(payload, marker, marker_len);
    payload[marker_len] = '\n';
    size_t total = marker_len + 1;

    int fd = open("/usb-persist.txt", O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "usb-boot-probe: open failed (errno=%d)\n", errno);
        return 1;
    }

    size_t written = 0;
    while (written < total) {
        ssize_t n = write(fd, payload + written, total - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "usb-boot-probe: write failed (errno=%d)\n", errno);
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "usb-boot-probe: short write\n");
            close(fd);
            return 1;
        }
        written += (size_t)n;
    }

    if (close(fd) != 0) {
        fprintf(stderr, "usb-boot-probe: close failed (errno=%d)\n", errno);
        return 1;
    }

    int64_t sync_rc = syscall(SYS_sync, 0, 0, 0);
    if (sync_rc != 0) {
        fprintf(stderr, "usb-boot-probe: sync failed (rc=%lld)\n", (long long)sync_rc);
        return 1;
    }

    printf("USB_WRITE_SYNC_OK\n");
    return 0;
}
