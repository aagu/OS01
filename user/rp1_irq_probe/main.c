#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static void print_hex(const uint8_t *buf, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        putchar(hex[(buf[i] >> 4) & 0xf]);
        putchar(hex[buf[i] & 0xf]);
    }
}

static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (!end || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int cmd_rx(const char *len_str)
{
    uint64_t want_u64 = 0;
    if (parse_u64(len_str, &want_u64) != 0 || want_u64 == 0 || want_u64 > 4096)
        return 2;
    size_t want = (size_t)want_u64;

    uint8_t *buf = malloc(want);
    if (!buf)
        return 1;

    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0) {
        free(buf);
        return 1;
    }
    raw = orig;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHOE | ECHOK | ECHONL);
    raw.c_iflag &= (tcflag_t)~(ICRNL | INLCR | IGNCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        free(buf);
        return 1;
    }

    printf("RP1-PROBE READY\n");
    fflush(stdout);

    size_t got = 0;
    int rc = 0;
    const int timeout_ms = 5000;
    while (got < want) {
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            rc = 1;
            break;
        }
        ssize_t n = read(STDIN_FILENO, buf + got, want - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            rc = 1;
            break;
        }
        got += (size_t)n;
    }

    (void)tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    if (rc == 0) {
        printf("RP1-PROBE RX ");
        print_hex(buf, want);
        printf("\n");
        fflush(stdout);
    }
    free(buf);
    return rc;
}

static int cmd_block(const char *dev, const char *lba_str)
{
    uint64_t lba = 0;
    if (!dev || !*dev || parse_u64(lba_str, &lba) != 0)
        return 2;
    if (lba > UINT64_MAX / 512ULL)
        return 2;

    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return 1;

    off_t off = (off_t)(lba * 512ULL);
    if (lseek(fd, off, SEEK_SET) != off) {
        close(fd);
        return 1;
    }

    uint8_t sec[512];
    size_t got = 0;
    while (got < sizeof(sec)) {
        ssize_t n = read(fd, sec + got, sizeof(sec) - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            close(fd);
            return 1;
        }
        got += (size_t)n;
    }
    close(fd);

    printf("RP1-PROBE BLOCK ");
    print_hex(sec, sizeof(sec));
    printf("\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "rx") == 0)
        return cmd_rx(argv[2]);
    if (argc == 4 && strcmp(argv[1], "block") == 0)
        return cmd_block(argv[2], argv[3]);
    fprintf(stderr, "usage: rp1-irq-probe rx LENGTH | block DEVICE LBA\n");
    return 2;
}
