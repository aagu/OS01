// user/test_arch10_capacity/test_arch10_capacity.c — Acceptance test for ARCH-10 capacity.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define PIPE_COUNT 16

static int get_handshake_state(int hfd)
{
    char buf[16];
    lseek(hfd, 0, SEEK_SET);
    int n = read(hfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return atoi(buf);
    }
    return 0;
}

static void test_poll(void)
{
    int pipes[PIPE_COUNT][2];
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (pipe(pipes[i]) != 0) {
            printf("pipe %d failed: errno=%d\n", i, errno);
            exit(1);
        }
    }

    int hfd = open("/dev/arch10-handshake", O_RDONLY);
    if (hfd < 0) {
        printf("open /dev/arch10-handshake failed\n");
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // Child writer: wait for parent to arm & sleep, then write to second half pipes (8..15)
        close(hfd);
        int ch_hfd = open("/dev/arch10-handshake", O_RDONLY);
        if (ch_hfd < 0) exit(1);

        // ── Phase 1: for poll32 ──
        while (get_handshake_state(ch_hfd) < 1) {
            // tight loop or yield
            for (volatile int d = 0; d < 1000; d++);
        }
        for (int i = 8; i < 16; i++) {
            if (write(pipes[i][1], "A", 1) != 1) {
                exit(1);
            }
        }

        // ── Phase 2: for poll64 ──
        while (get_handshake_state(ch_hfd) < 2) {
            for (volatile int d = 0; d < 1000; d++);
        }
        for (int i = 8; i < 16; i++) {
            if (write(pipes[i][1], "B", 1) != 1) {
                exit(1);
            }
        }

        close(ch_hfd);
        for (int i = 0; i < PIPE_COUNT; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        exit(0);
    }

    // ── Parent poller ──

    // 1. poll32 (32 entries)
    struct pollfd pfd32[32];
    // First 16 entries -> pipes 0..7
    for (int i = 0; i < 16; i++) {
        pfd32[i].fd = pipes[i % 8][0];
        pfd32[i].events = POLLIN;
        pfd32[i].revents = 0;
    }
    // Second 16 entries -> pipes 8..15
    for (int i = 16; i < 32; i++) {
        pfd32[i].fd = pipes[8 + (i % 8)][0];
        pfd32[i].events = POLLIN;
        pfd32[i].revents = 0;
    }

    int rc32 = poll(pfd32, 32, -1);
    if (rc32 <= 0) {
        printf("poll32 failed: rc=%d, errno=%d\n", rc32, errno);
        exit(1);
    }

    for (int i = 0; i < 16; i++) {
        if (pfd32[i].revents != 0) {
            printf("poll32 first half entry %d unexpectedly ready: revents=%d\n", i, pfd32[i].revents);
            exit(1);
        }
    }
    for (int i = 16; i < 32; i++) {
        if (!(pfd32[i].revents & POLLIN)) {
            printf("poll32 second half entry %d missing POLLIN: revents=%d\n", i, pfd32[i].revents);
            exit(1);
        }
    }
    // Drain byte from pipes 8..15
    for (int i = 8; i < 16; i++) {
        char dummy;
        if (read(pipes[i][0], &dummy, 1) != 1) {
            printf("drain pipe %d failed\n", i);
            exit(1);
        }
    }
    printf("[arch10-capacity] poll32 PASS\n");

    // 2. poll64 (64 entries)
    struct pollfd pfd64[64];
    // First 32 entries -> pipes 0..7
    for (int i = 0; i < 32; i++) {
        pfd64[i].fd = pipes[i % 8][0];
        pfd64[i].events = POLLIN;
        pfd64[i].revents = 0;
    }
    // Second 32 entries -> pipes 8..15
    for (int i = 32; i < 64; i++) {
        pfd64[i].fd = pipes[8 + (i % 8)][0];
        pfd64[i].events = POLLIN;
        pfd64[i].revents = 0;
    }

    int rc64 = poll(pfd64, 64, -1);
    if (rc64 <= 0) {
        printf("poll64 failed: rc=%d, errno=%d\n", rc64, errno);
        exit(1);
    }

    for (int i = 0; i < 32; i++) {
        if (pfd64[i].revents != 0) {
            printf("poll64 first half entry %d unexpectedly ready: revents=%d\n", i, pfd64[i].revents);
            exit(1);
        }
    }
    for (int i = 32; i < 64; i++) {
        if (!(pfd64[i].revents & POLLIN)) {
            printf("poll64 second half entry %d missing POLLIN: revents=%d\n", i, pfd64[i].revents);
            exit(1);
        }
    }
    // Drain
    for (int i = 8; i < 16; i++) {
        char dummy;
        read(pipes[i][0], &dummy, 1);
    }
    printf("[arch10-capacity] poll64 PASS\n");

    int status = 0;
    waitpid(pid, &status, 0);

    close(hfd);
    for (int i = 0; i < PIPE_COUNT; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

static void test_limits(void)
{
    struct rlimit orig;
    if (getrlimit(RLIMIT_NOFILE, &orig) != 0) {
        printf("getrlimit failed: errno=%d\n", errno);
        exit(1);
    }
    if (orig.rlim_cur != 64 || orig.rlim_max != 64) {
        printf("initial limit expected 64/64, got %lu/%lu\n", orig.rlim_cur, orig.rlim_max);
        exit(1);
    }

    // Open a high fd
    int high_fd = dup2(1, 20);
    if (high_fd != 20) {
        printf("dup2 to fd 20 failed: rc=%d, errno=%d\n", high_fd, errno);
        exit(1);
    }

    // Lower soft limit to 4
    struct rlimit low = { .rlim_cur = 4, .rlim_max = 64 };
    if (setrlimit(RLIMIT_NOFILE, &low) != 0) {
        printf("setrlimit to 4 failed: errno=%d\n", errno);
        exit(1);
    }

    // 1. poll with 65 items (must fail EINVAL)
    struct pollfd pfds65[65];
    for (int i = 0; i < 65; i++) {
        pfds65[i].fd = 1;
        pfds65[i].events = POLLOUT;
        pfds65[i].revents = 0;
    }
    int prc = poll(pfds65, 65, 0);
    if (prc != -1 || errno != EINVAL) {
        printf("poll 65 items with soft=4 did not fail with EINVAL: prc=%d errno=%d\n", prc, errno);
        exit(1);
    }

    // 2. Occupy fd 3 (< 4), then next new fd installation (fd 4 >= 4) must fail EMFILE
    int fd3 = open("/dev/null", O_RDONLY);
    if (fd3 < 0) {
        printf("open fd 3 failed: errno=%d\n", errno);
        exit(1);
    }
    int nfd = open("/dev/null", O_RDONLY);
    if (nfd != -1 || errno != EMFILE) {
        printf("open new fd with soft=4 did not fail with EMFILE: nfd=%d errno=%d\n", nfd, errno);
        exit(1);
    }
    close(fd3);

    // 3. Already open high fd (20) continues to be accessible
    int flags = fcntl(high_fd, F_GETFL);
    if (flags < 0) {
        printf("high fd 20 fcntl failed: rc=%d errno=%d\n", flags, errno);
        exit(1);
    }

    // 4. Restore soft limit to original hard limit
    if (setrlimit(RLIMIT_NOFILE, &orig) != 0) {
        printf("setrlimit restore failed: errno=%d\n", errno);
        exit(1);
    }

    close(high_fd);
    printf("[arch10-capacity] limit PASS\n");
}

static void test_block12(void)
{
    for (int i = 0; i < 12; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fakeblk%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("open %s failed: fd=%d errno=%d\n", path, fd, errno);
            exit(1);
        }
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            printf("read %s failed: n=%d errno=%d\n", path, n, errno);
            exit(1);
        }
        char expected[32];
        snprintf(expected, sizeof(expected), "FAKEBLK_PAYLOAD_%02d", i);
        if (!strstr(buf, expected)) {
            printf("payload mismatch for %s: got '%s', expected '%s'\n", path, buf, expected);
            exit(1);
        }
    }
    printf("[arch10-capacity] block12 PASS\n");
}

static void test_devfs40(void)
{
    for (int i = 0; i < 40; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/arch10-node-%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("open %s failed: fd=%d errno=%d\n", path, fd, errno);
            exit(1);
        }
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            printf("read %s failed: n=%d errno=%d\n", path, n, errno);
            exit(1);
        }
        char expected[32];
        snprintf(expected, sizeof(expected), "ARCH10_NODE_PAYLOAD_%02d", i);
        if (!strstr(buf, expected)) {
            printf("payload mismatch for %s: got '%s', expected '%s'\n", path, buf, expected);
            exit(1);
        }
    }
    printf("[arch10-capacity] devfs40 PASS\n");
}

int main(void)
{
    printf("[arch10-capacity] starting tests...\n");

    test_poll();
    test_limits();
    test_block12();
    test_devfs40();

    printf("[arch10-capacity] PASS\n");
    return 0;
}
