#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gfx.h>
#include <uapi/fb.h>

static int run_ebusy(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: doom_testctl ebusy <cmd> [args...]\n");
        return 1;
    }

    /* Open display and acquire ownership to occupy screen */
    int fb_fd = open("/dev/fb", O_RDONLY);
    if (fb_fd < 0) {
        perror("doom_testctl: open /dev/fb");
        return 1;
    }
    struct fb_info info;
    if (read(fb_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
        perror("doom_testctl: read fb_info");
        close(fb_fd);
        return 1;
    }
    close(fb_fd);

    gfx_handle_t *h = gfx_open(0, 0, info.width, info.height);
    if (!h) {
        perror("doom_testctl: gfx_open");
        return 1;
    }
    if (gfx_acquire(h) < 0) {
        perror("doom_testctl: gfx_acquire");
        gfx_close(h);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("doom_testctl: fork");
        gfx_release(h);
        gfx_close(h);
        return 1;
    }

    if (pid == 0) {
        execv(argv[2], &argv[2]);
        perror("doom_testctl: execv child");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    gfx_release(h);
    gfx_close(h);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        printf("[testctl] ebusy: child %d exited with code %d as expected\n", (int)pid, WEXITSTATUS(status));
        return 0;
    }

    printf("[testctl] ebusy: child %d did not fail with error (status=%d)\n", (int)pid, status);
    return 1;
}

static int run_sigkill(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: doom_testctl sigkill <cmd> [args...]\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("doom_testctl: fork");
        return 1;
    }

    if (pid == 0) {
        execv(argv[2], &argv[2]);
        perror("doom_testctl: execv child");
        _exit(127);
    }

    /* Wait 2 seconds for Doom to reach running state */
    sleep(2);

    kill(pid, SIGKILL);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
        printf("[testctl] sigkill: child %d killed by signal %d\n", (int)pid, WTERMSIG(status));
        return 0;
    }

    printf("[testctl] sigkill: child %d unexpected status %d\n", (int)pid, status);
    return 1;
}

static int run_mode_switch(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: doom_testctl mode-switch <cmd> [args...]\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("doom_testctl: fork");
        return 1;
    }

    if (pid == 0) {
        execv(argv[2], &argv[2]);
        perror("doom_testctl: execv child");
        _exit(127);
    }

    /* Wait 2 seconds for Doom to reach running state and render first frame */
    sleep(2);

    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) {
        perror("doom_testctl: open /dev/fb for mode switch");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    struct fb_state st;
    memset(&st, 0, sizeof(st));
    if (ioctl(fb_fd, FBIOGET_STATE, &st) < 0) {
        perror("doom_testctl: FBIOGET_STATE");
        close(fb_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }
    uint64_t init_gen = st.generation;

    struct fb_modes_req req;
    memset(&req, 0, sizeof(req));
    req.capacity = FB_MAX_MODES;
    if (ioctl(fb_fd, FBIOGET_MODES, &req) < 0) {
        perror("doom_testctl: FBIOGET_MODES");
        close(fb_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    uint32_t target_w = 0, target_h = 0;
    for (uint32_t i = 0; i < req.count; i++) {
        if (req.modes[i].width != st.info.width || req.modes[i].height != st.info.height) {
            target_w = req.modes[i].width;
            target_h = req.modes[i].height;
            break;
        }
    }

    if (target_w == 0 || target_h == 0) {
        fprintf(stderr, "doom_testctl: no alternative mode found in %u modes\n", req.count);
        close(fb_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    struct fb_set_mode_req sreq;
    memset(&sreq, 0, sizeof(sreq));
    sreq.width = target_w;
    sreq.height = target_h;
    sreq.bpp = 0;
    if (ioctl(fb_fd, FBIOSET_MODE, &sreq) < 0) {
        perror("doom_testctl: FBIOSET_MODE");
        close(fb_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    struct fb_state st2;
    memset(&st2, 0, sizeof(st2));
    if (ioctl(fb_fd, FBIOGET_STATE, &st2) < 0 || st2.generation <= init_gen) {
        fprintf(stderr, "doom_testctl: generation did not increment (%llu -> %llu)\n",
                (unsigned long long)init_gen, (unsigned long long)st2.generation);
        close(fb_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    close(fb_fd);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        printf("[testctl] mode-switch: child %d exited with code %d\n", (int)pid, WEXITSTATUS(status));
        return 0;
    }

    printf("[testctl] mode-switch: child %d did not fail with error (status=%d)\n", (int)pid, status);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: doom_testctl ebusy|sigkill|mode-switch <cmd> [args...]\n");
        return 1;
    }

    if (strcmp(argv[1], "ebusy") == 0) {
        return run_ebusy(argc, argv);
    } else if (strcmp(argv[1], "sigkill") == 0) {
        return run_sigkill(argc, argv);
    } else if (strcmp(argv[1], "mode-switch") == 0) {
        return run_mode_switch(argc, argv);
    }

    fprintf(stderr, "doom_testctl: unknown action '%s'\n", argv[1]);
    return 1;
}
