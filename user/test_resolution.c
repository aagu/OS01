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
#include <sys/ioctl.h>

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

static int cmd_hold(void)
{
    struct fb_test_req req;
    fill_req(&req, 0, 0);
    if (ioctl(g_fbtest_fd, FBIOTEST_HOLD_WRITER, &req) < 0) {
        printf("ERROR=hold errno=%d\n", errno);
        return 1;
    }
    printf("HOLD ok\n");
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
           "hold|release|arm-terminal PID TOKEN|consume-terminal PID>\n");
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
    } else if (!strcmp(cmd, "hold")) {
        rc = cmd_hold();
    } else if (!strcmp(cmd, "release")) {
        rc = cmd_release();
    } else if (!strcmp(cmd, "arm-terminal") && argc == 4) {
        rc = cmd_arm_terminal((uint32_t)strtoul(argv[2], NULL, 10),
                              (uint64_t)strtoull(argv[3], NULL, 0));
    } else if (!strcmp(cmd, "consume-terminal") && argc == 3) {
        rc = cmd_consume_terminal((uint32_t)strtoul(argv[2], NULL, 10));
    } else {
        usage();
        rc = 2;
    }

    return rc;
}
