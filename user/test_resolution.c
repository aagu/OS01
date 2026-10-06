/*
 * user/test_resolution.c — QEMU resolution switcher fault-injection helper.
 *
 * Packaged ONLY in the FB_RESOLUTION_TEST=1 variant (rootfs + USER_PROGRAMS
 * are gated on that profile).  It drives /dev/fbtest to arm one-shot faults
 * and snapshots the live hardware registers, and prints machine-readable
 * KEY=VALUE lines for the QEMU runner (Task 10).
 *
 * It never writes DISPI ports directly: the only control surface is the
 * fixed ioctl ABI in <uapi/fb_test.h>.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>   /* struct winsize */
#include <sys/mman.h>

#include <uapi/fb.h>
#include <uapi/fb_test.h>

static int g_fbtest_fd = -1;
static int g_fb_fd = -1;

static int open_nodes(void)
{
    g_fbtest_fd = open("/dev/fbtest", O_RDWR);
    if (g_fbtest_fd < 0) {
        printf("ERROR=fbtest_open errno=%d\n", errno);
        return -1;
    }
    g_fb_fd = open("/dev/fb", O_RDWR);
    if (g_fb_fd < 0) {
        printf("ERROR=fb_open errno=%d\n", errno);
        return -1;
    }
    return 0;
}

static void fill_req(struct fb_test_req *req, uint32_t pid, uint64_t token)
{
    memset(req, 0, sizeof(*req));
    req->version = 1;
    req->target_pid = pid;
    req->token = token;
}

static int cmd_snapshot(void)
{
    struct fb_test_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    if (ioctl(g_fbtest_fd, FBIOTEST_SNAPSHOT, &snap) < 0) {
        printf("ERROR=snapshot errno=%d\n", errno);
        return 1;
    }
    printf("STATE width=%u height=%u stride=%u bpp=%u format=%u generation=%llu\n",
           snap.state.info.width, snap.state.info.height, snap.state.info.stride,
           snap.state.info.bpp, snap.state.info.format,
           (unsigned long long)snap.state.generation);
    printf("REGS");
    for (int i = 0; i < 11; i++) {
        printf(" %u", snap.regs[i]);
    }
    printf("\n");
    printf("ACTIVE_WRITERS=%llu\n", (unsigned long long)snap.active_writers);
    return 0;
}

static int cmd_get(void)
{
    struct fb_state st;
    memset(&st, 0, sizeof(st));
    if (ioctl(g_fb_fd, FBIOGET_STATE, &st) < 0) {
        printf("ERROR=get_state errno=%d\n", errno);
        return 1;
    }
    printf("GET width=%u height=%u generation=%llu\n",
           st.info.width, st.info.height, (unsigned long long)st.generation);
    return 0;
}

static int cmd_set(uint32_t w, uint32_t h)
{
    struct fb_set_mode_req req = { .width = w, .height = h, .bpp = 0 };
    int rc = ioctl(g_fb_fd, FBIOSET_MODE, &req);
    printf("SET %ux%u rc=%d errno=%d\n", w, h, rc, rc < 0 ? errno : 0);
    return rc < 0 ? 1 : 0;
}

static int arm_register(uint32_t cmd, uint32_t pid, uint64_t token,
                        uint32_t w, uint32_t h)
{
    struct fb_test_req req;
    fill_req(&req, pid, token);
    if (ioctl(g_fbtest_fd, cmd, &req) < 0) {
        printf("ERROR=arm errno=%d\n", errno);
        return 1;
    }
    return cmd_set(w, h);
}

static int cmd_hold(int secs)
{
    struct fb_test_req req;
    fill_req(&req, 0, 0);
    if (ioctl(g_fbtest_fd, FBIOTEST_HOLD_WRITER, &req) < 0) {
        printf("ERROR=hold errno=%d\n", errno);
        return 1;
    }
    printf("HOLD ok secs=%d\n", secs);
    fflush(stdout);

    /* With a duration, keep the real writer lease held across the window in
     * which the runner issues a SET, so the SET observes the 1 s drain
     * timeout.  The lease is released explicitly below (and again by
     * release_file on exit). */
    for (int i = 0; i < secs; i++)
        sleep(1);

    if (ioctl(g_fbtest_fd, FBIOTEST_RELEASE_WRITER, &req) < 0) {
        printf("ERROR=release errno=%d\n", errno);
        return 1;
    }
    printf("RELEASE ok\n");
    return 0;
}

/* pty-watch: foreground SIGWINCH observer (spec §8.2 item 4).  Installs a
 * SIGWINCH handler and prints a versioned JSON line per signal carrying the
 * four TIOCGWINSZ fields.  Exits after <secs>. */
static volatile int g_winch;

static void winch_handler(int sig)
{
    (void)sig;
    g_winch = 1;
}

static void emit_winsize(int sigwinch)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    (void)ioctl(0, TIOCGWINSZ, &ws);
    printf("RESJSON {\"v\": 1, \"op\": \"pty-watch\", \"sigwinch\": %d, "
           "\"row\": %u, \"col\": %u, \"xpixel\": %u, \"ypixel\": %u}\n",
           sigwinch, (unsigned)ws.ws_row, (unsigned)ws.ws_col,
           (unsigned)ws.ws_xpixel, (unsigned)ws.ws_ypixel);
    fflush(stdout);
}

static int cmd_pty_watch(int secs, uint32_t setw, uint32_t seth)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = winch_handler;
    sigaction(SIGWINCH, &sa, NULL);

    /* Foreground process: fd 0 is the PTY slave, so TIOCGWINSZ reflects the
     * terminal's own geometry.  The observer drives the switch itself (the
     * runner keeps it in the foreground, so no separate process could type
     * the command concurrently). */
    emit_winsize(0);
    if (setw && seth)
        (void)cmd_set(setw, seth);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (g_winch) {
            g_winch = 0;
            emit_winsize(1);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((int)(t1.tv_sec - t0.tv_sec) >= secs)
            break;
        poll(NULL, 0, 50);
    }
    return 0;
}

/* surrender: exercise the no-BGA FBIOSURRENDER path (spec §8.2 item 6).  It
 * takes no argument and must succeed even when no switchable backend exists. */
static int cmd_surrender(void)
{
    int fd = open("/dev/fb", O_RDWR);
    if (fd < 0) {
        printf("ERROR=fb_open errno=%d\n", errno);
        return 1;
    }
    int rc = ioctl(fd, FBIOSURRENDER, NULL);
    int e = rc < 0 ? errno : 0;
    close(fd);
    printf("SURRENDER rc=%d errno=%d\n", rc, e);
    return rc < 0 ? 1 : 0;
}

/* mmap: take a raw shared mapping of /dev/fb once, then exit.  The sticky
 * raw_mmap_seen flag makes every subsequent layout SET return EBUSY for the
 * rest of the boot (spec §3.3 / §8.2 item 6). */
static int cmd_mmap(void)
{
    int fd = open("/dev/fb", O_RDWR);
    if (fd < 0) {
        printf("RESJSON {\"v\": 1, \"op\": \"mmap\", \"rc\": -1, \"errno\": %d}\n",
               errno);
        return 1;
    }
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("RESJSON {\"v\": 1, \"op\": \"mmap\", \"rc\": -1, \"errno\": %d}\n",
               errno);
        close(fd);
        return 1;
    }
    printf("RESJSON {\"v\": 1, \"op\": \"mmap\", \"rc\": 0, \"bytes\": 4096}\n");
    fflush(stdout);
    munmap(p, 4096);
    close(fd);
    return 0;
}

static int cmd_release(void)
{
    struct fb_test_req req;
    fill_req(&req, 0, 0);
    if (ioctl(g_fbtest_fd, FBIOTEST_RELEASE_WRITER, &req) < 0) {
        printf("ERROR=release errno=%d\n", errno);
        return 1;
    }
    printf("RELEASE ok\n");
    return 0;
}

static int cmd_arm_terminal(uint32_t pid, uint64_t token)
{
    struct fb_test_req req;
    fill_req(&req, pid, token);
    if (ioctl(g_fbtest_fd, FBIOTEST_ARM_TERMINAL_ENOMEM, &req) < 0) {
        printf("ERROR=arm_terminal errno=%d\n", errno);
        return 1;
    }
    printf("ARM_TERMINAL pid=%u ok\n", pid);
    return 0;
}

static int cmd_consume_terminal(uint32_t pid)
{
    struct fb_test_req req;
    fill_req(&req, pid, 0);
    int rc = ioctl(g_fbtest_fd, FBIOTEST_CONSUME_TERMINAL_ENOMEM, &req);
    printf("CONSUME_TERMINAL pid=%u rc=%d\n", pid, rc);
    return rc < 0 ? 1 : 0;
}

static void usage(void)
{
    printf("usage: test_resolution <snapshot|get|set W H|"
           "arm-mismatch PID TOKEN W H|arm-rollback PID TOKEN W H|"
           "hold [SECS]|release|arm-terminal PID TOKEN|consume-terminal PID|"
           "pty-watch [SECS [W H]]|mmap|surrender>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (open_nodes() != 0) {
        return 1;
    }

    const char *cmd = argv[1];
    int rc = 0;

    if (!strcmp(cmd, "snapshot")) {
        rc = cmd_snapshot();
    } else if (!strcmp(cmd, "get")) {
        rc = cmd_get();
    } else if (!strcmp(cmd, "set") && argc == 4) {
        rc = cmd_set((uint32_t)strtoul(argv[2], NULL, 10),
                     (uint32_t)strtoul(argv[3], NULL, 10));
    } else if (!strcmp(cmd, "arm-mismatch") && argc == 6) {
        rc = arm_register(FBIOTEST_ARM_MISMATCH,
                          (uint32_t)strtoul(argv[2], NULL, 10),
                          (uint64_t)strtoull(argv[3], NULL, 0),
                          (uint32_t)strtoul(argv[4], NULL, 10),
                          (uint32_t)strtoul(argv[5], NULL, 10));
    } else if (!strcmp(cmd, "arm-rollback") && argc == 6) {
        rc = arm_register(FBIOTEST_ARM_ROLLBACK_FAILURE,
                          (uint32_t)strtoul(argv[2], NULL, 10),
                          (uint64_t)strtoull(argv[3], NULL, 0),
                          (uint32_t)strtoul(argv[4], NULL, 10),
                          (uint32_t)strtoul(argv[5], NULL, 10));
    } else if (!strcmp(cmd, "hold") && (argc == 2 || argc == 3)) {
        rc = cmd_hold(argc == 3 ? (int)strtol(argv[2], NULL, 10) : 0);
    } else if (!strcmp(cmd, "release")) {
        rc = cmd_release();
    } else if (!strcmp(cmd, "arm-terminal") && argc == 4) {
        rc = cmd_arm_terminal((uint32_t)strtoul(argv[2], NULL, 10),
                              (uint64_t)strtoull(argv[3], NULL, 0));
    } else if (!strcmp(cmd, "consume-terminal") && argc == 3) {
        rc = cmd_consume_terminal((uint32_t)strtoul(argv[2], NULL, 10));
    } else if (!strcmp(cmd, "pty-watch") &&
               (argc == 2 || argc == 3 || argc == 4)) {
        uint32_t pw = 0, ph = 0;
        if (argc == 4)
            (void)sscanf(argv[3], "%ux%u", &pw, &ph);
        rc = cmd_pty_watch(argc >= 3 ? (int)strtol(argv[2], NULL, 10) : 6,
                           pw, ph);
    } else if (!strcmp(cmd, "mmap")) {
        rc = cmd_mmap();
    } else if (!strcmp(cmd, "surrender")) {
        rc = cmd_surrender();
    } else {
        usage();
        rc = 2;
    }

    return rc;
}
