/* user/test_display_owner/test_display_owner.c — guest test program for display ownership contract.
 *
 * Subcommands:
 *   hold              Acquire display ownership, print READY, and sleep.
 *   kill-owner        Parent forks child who acquires ownership; parent SIGKILLs child and waitpid.
 *   dup-close         Acquire ownership, dup(fd), close handle, verify retained, close dup, verify restored.
 *   fork-close        Acquire in parent, fork child who gfx_close()s inherited handle; verify parent retained.
 *   close-before-exec Fork child who closes handle before execve; verify parent retains ownership.
 *   readonly-write    Mmap /dev/fb with PROT_READ; child write attempt must terminate with signal.
 *   readonly-mprotect Mmap /dev/fb with PROT_READ; mprotect upgrade returns EPERM and mapping stays readonly.
 *   present-fault     Acquire ownership, trigger GFX_PRESENT fault, verify lease released on close.
 *   all               Run all self-contained subcommands above (excluding hold).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <gfx.h>
#include <uapi/fb.h>
#include <uapi/gfx.h>

static int get_screen_dimensions(uint32_t *out_w, uint32_t *out_h)
{
    int fd = open("/dev/fb", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/fb");
        return -1;
    }
    struct fb_info info;
    ssize_t r = read(fd, &info, sizeof(info));
    close(fd);
    if (r != (ssize_t)sizeof(info) || info.width == 0 || info.height == 0) {
        fprintf(stderr, "invalid /dev/fb metadata\n");
        return -1;
    }
    *out_w = info.width;
    *out_h = info.height;
    return 0;
}

static int get_handle_fd(const gfx_handle_t *gh)
{
    /* In libgfx, struct gfx_handle has int fd as its first field at offset 0 */
    return *(const int *)gh;
}

/* 1. hold: Acquire ownership, print READY, and sleep indefinitely */
static int cmd_hold(void)
{
    uint32_t w, h;
    if (get_screen_dimensions(&w, &h) != 0) {
        return 1;
    }
    gfx_handle_t *gh = gfx_open(0, 0, w, h);
    if (!gh) {
        fprintf(stderr, "hold: gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    if (gfx_acquire(gh) != 0) {
        fprintf(stderr, "hold: gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh);
        return 1;
    }
    printf("READY\n");
    fflush(stdout);
    while (1) {
        sleep(10);
    }
    return 0;
}

/* 2. kill-owner: Fork child that acquires ownership, parent SIGKILLs and waitpid */
static int cmd_kill_owner(void)
{
    uint32_t w, h;
    if (get_screen_dimensions(&w, &h) != 0) {
        return 1;
    }

    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pfd[0]);
        close(pfd[1]);
        return 1;
    }

    if (pid == 0) {
        close(pfd[0]);
        gfx_handle_t *gh = gfx_open(0, 0, w, h);
        if (!gh) {
            write(pfd[1], "E", 1);
            _exit(1);
        }
        if (gfx_acquire(gh) != 0) {
            write(pfd[1], "E", 1);
            _exit(1);
        }
        write(pfd[1], "R", 1);
        close(pfd[1]);
        while (1) {
            sleep(10);
        }
        _exit(0);
    }

    close(pfd[1]);
    char ch = 0;
    if (read(pfd[0], &ch, 1) != 1 || ch != 'R') {
        fprintf(stderr, "kill-owner: child failed to acquire ownership\n");
        close(pfd[0]);
        waitpid(pid, NULL, 0);
        return 1;
    }
    close(pfd[0]);

    if (kill(pid, SIGKILL) != 0) {
        perror("kill");
        return 1;
    }

    int status = 0;
    pid_t wr = waitpid(pid, &status, 0);
    if (wr != pid) {
        perror("waitpid");
        return 1;
    }

    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        fprintf(stderr, "kill-owner: child not terminated by SIGKILL (status=0x%x)\n", status);
        return 1;
    }

    printf("[TEST_DISPLAY_OWNER] kill-owner PASS\n");
    return 0;
}

/* 3. dup-close: Acquire ownership, dup(fd), close handle, verify retained, close dup */
static int cmd_dup_close(void)
{
    uint32_t w, h;
    if (get_screen_dimensions(&w, &h) != 0) {
        return 1;
    }

    gfx_handle_t *gh = gfx_open(0, 0, w, h);
    if (!gh) {
        fprintf(stderr, "dup-close: gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    if (gfx_acquire(gh) != 0) {
        fprintf(stderr, "dup-close: gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh);
        return 1;
    }

    gfx_display_state_t st;
    if (gfx_get_display_state(gh, &st) != 0) {
        fprintf(stderr, "dup-close: gfx_get_display_state failed (errno=%d)\n", errno);
        gfx_close(gh);
        return 1;
    }
    if (!(st.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        fprintf(stderr, "dup-close: not foreground after acquire\n");
        gfx_close(gh);
        return 1;
    }
    uint64_t gen1 = st.generation;

    int fd = get_handle_fd(gh);
    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        perror("dup");
        gfx_close(gh);
        return 1;
    }

    /* Close original handle. Because dup_fd is open, final close is not reached yet */
    gfx_close(gh);

    /* Verify ownership is retained across dup_fd */
    gfx_display_state_t st2;
    if (ioctl(dup_fd, GFX_GET_DISPLAY_STATE, (uint64_t)&st2) != 0) {
        fprintf(stderr, "dup-close: ioctl on dup_fd failed (errno=%d)\n", errno);
        close(dup_fd);
        return 1;
    }
    if (!(st2.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        fprintf(stderr, "dup-close: dup_fd lost foreground before close\n");
        close(dup_fd);
        return 1;
    }
    if (st2.generation != gen1) {
        fprintf(stderr, "dup-close: generation changed unexpectedly while dup open\n");
        close(dup_fd);
        return 1;
    }

    /* Close the duplicated fd: triggers final close in kernel */
    close(dup_fd);

    /* Probe that final close restored fallback (generation bumped) */
    gfx_handle_t *probe = gfx_open(0, 0, w, h);
    if (!probe) {
        fprintf(stderr, "dup-close: probe gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    gfx_display_state_t st3;
    if (gfx_get_display_state(probe, &st3) != 0) {
        fprintf(stderr, "dup-close: probe get_state failed\n");
        gfx_close(probe);
        return 1;
    }
    gfx_close(probe);

    if (st3.generation <= gen1) {
        fprintf(stderr, "dup-close: generation not incremented on final close (old=%llu, new=%llu)\n",
                (unsigned long long)gen1, (unsigned long long)st3.generation);
        return 1;
    }

    printf("[TEST_DISPLAY_OWNER] dup-close PASS\n");
    return 0;
}

/* 4. fork-close: Child explicitly closes inherited handle; parent retains ownership */
static int cmd_fork_close(void)
{
    uint32_t w, h;
    if (get_screen_dimensions(&w, &h) != 0) {
        return 1;
    }

    gfx_handle_t *gh = gfx_open(0, 0, w, h);
    if (!gh) {
        fprintf(stderr, "fork-close: gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    if (gfx_acquire(gh) != 0) {
        fprintf(stderr, "fork-close: gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh);
        return 1;
    }

    gfx_display_state_t st1;
    if (gfx_get_display_state(gh, &st1) != 0 || !(st1.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        fprintf(stderr, "fork-close: parent not foreground\n");
        gfx_close(gh);
        return 1;
    }

    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("pipe");
        gfx_close(gh);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pfd[0]);
        close(pfd[1]);
        gfx_close(gh);
        return 1;
    }

    if (pid == 0) {
        close(pfd[0]);
        /* Child explicitly closes inherited graphics handle */
        gfx_close(gh);
        write(pfd[1], "K", 1);
        close(pfd[1]);
        _exit(0);
    }

    close(pfd[1]);
    char ch = 0;
    if (read(pfd[0], &ch, 1) != 1 || ch != 'K') {
        fprintf(stderr, "fork-close: child failed to close handle\n");
        close(pfd[0]);
        waitpid(pid, NULL, 0);
        gfx_close(gh);
        return 1;
    }
    close(pfd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "fork-close: child exited abnormally\n");
        gfx_close(gh);
        return 1;
    }

    /* Verify parent still retains foreground ownership */
    gfx_display_state_t st2;
    if (gfx_get_display_state(gh, &st2) != 0) {
        fprintf(stderr, "fork-close: get_state failed\n");
        gfx_close(gh);
        return 1;
    }
    if (!(st2.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        fprintf(stderr, "fork-close: parent lost foreground after child closed inherited fd\n");
        gfx_close(gh);
        return 1;
    }

    gfx_release(gh);
    gfx_close(gh);

    printf("[TEST_DISPLAY_OWNER] fork-close PASS\n");
    return 0;
}

/* 5. close-before-exec: Explicitly close inherited handle before execve */
static int cmd_close_before_exec(void)
{
    uint32_t w, h;
    if (get_screen_dimensions(&w, &h) != 0) {
        return 1;
    }

    gfx_handle_t *gh = gfx_open(0, 0, w, h);
    if (!gh) {
        fprintf(stderr, "close-before-exec: gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    if (gfx_acquire(gh) != 0) {
        fprintf(stderr, "close-before-exec: gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh);
        return 1;
    }

    gfx_display_state_t st1;
    if (gfx_get_display_state(gh, &st1) != 0 || !(st1.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        fprintf(stderr, "close-before-exec: parent not foreground\n");
        gfx_close(gh);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        gfx_close(gh);
        return 1;
    }

    if (pid == 0) {
        /* Child closes inherited handle explicitly before exec */
        gfx_close(gh);
        char *const argv[] = { "test_display_owner", "exec-target", NULL };
        execv("/bin/test_display_owner", argv);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "close-before-exec: child exec target failed\n");
        gfx_close(gh);
        return 1;
    }

    /* Verify parent still retains foreground ownership */
    gfx_display_state_t st2;
    if (gfx_get_display_state(gh, &st2) != 0 || !(st2.flags & GFX_DISPLAY_STATE_FOREGROUND)) {
        fprintf(stderr, "close-before-exec: parent lost foreground after child exec\n");
        gfx_close(gh);
        return 1;
    }

    gfx_release(gh);
    gfx_close(gh);

    printf("[TEST_DISPLAY_OWNER] close-before-exec PASS\n");
    return 0;
}

/* 6. readonly-write: Child attempts to write to PROT_READ /dev/fb mmap, terminated by signal */
static int cmd_readonly_write(void)
{
    int fb_fd = open("/dev/fb", O_RDONLY);
    if (fb_fd < 0) {
        perror("open /dev/fb");
        return 1;
    }

    struct fb_info info;
    if (read(fb_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
        fprintf(stderr, "readonly-write: read info failed\n");
        close(fb_fd);
        return 1;
    }

    size_t bytes = (size_t)info.height * info.stride;
    void *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "readonly-write: mmap PROT_READ failed (errno=%d)\n", errno);
        close(fb_fd);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        munmap(p, bytes);
        close(fb_fd);
        return 1;
    }

    if (pid == 0) {
        /* Attempt to write into read-only mapped page */
        *(volatile uint32_t *)p = 0x12345678;
        _exit(0); /* Should not be reached */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    munmap(p, bytes);
    close(fb_fd);

    if (!WIFSIGNALED(status)) {
        fprintf(stderr, "readonly-write: child was NOT terminated by signal (status=0x%x)\n", status);
        return 1;
    }

    printf("[TEST_DISPLAY_OWNER] readonly-write PASS\n");
    return 0;
}

/* 7. readonly-mprotect: mprotect PROT_READ|PROT_WRITE returns EPERM and mapping remains readonly */
static int cmd_readonly_mprotect(void)
{
    int fb_fd = open("/dev/fb", O_RDONLY);
    if (fb_fd < 0) {
        perror("open /dev/fb");
        return 1;
    }

    struct fb_info info;
    if (read(fb_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
        fprintf(stderr, "readonly-mprotect: read info failed\n");
        close(fb_fd);
        return 1;
    }

    size_t bytes = (size_t)info.height * info.stride;
    void *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "readonly-mprotect: mmap PROT_READ failed (errno=%d)\n", errno);
        close(fb_fd);
        return 1;
    }

    int rc = mprotect(p, bytes, PROT_READ | PROT_WRITE);
    if (rc != -1 || errno != EPERM) {
        fprintf(stderr, "readonly-mprotect: mprotect upgrade returned %d errno=%d, expected -1 EPERM\n",
                rc, errno);
        munmap(p, bytes);
        close(fb_fd);
        return 1;
    }

    /* Confirm mapping is still read-only by verifying child write is terminated by signal */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        munmap(p, bytes);
        close(fb_fd);
        return 1;
    }

    if (pid == 0) {
        *(volatile uint32_t *)p = 0x87654321;
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    munmap(p, bytes);
    close(fb_fd);

    if (!WIFSIGNALED(status)) {
        fprintf(stderr, "readonly-mprotect: child write succeeded after failed mprotect (status=0x%x)\n", status);
        return 1;
    }

    printf("[TEST_DISPLAY_OWNER] readonly-mprotect PASS\n");
    return 0;
}

/* 8. present-fault: Acquire ownership, trigger GFX_PRESENT fault, verify lease released on close */
static int cmd_present_fault(void)
{
    uint32_t w, h;
    if (get_screen_dimensions(&w, &h) != 0) {
        return 1;
    }

    gfx_handle_t *gh = gfx_open(0, 0, w, h);
    if (!gh) {
        fprintf(stderr, "present-fault: gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    if (gfx_acquire(gh) != 0) {
        fprintf(stderr, "present-fault: gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh);
        return 1;
    }

    int fd = get_handle_fd(gh);
    /* Trigger present fault by passing NULL user pointer to GFX_PRESENT */
    int prc = ioctl(fd, GFX_PRESENT, (uint64_t)0);
    if (prc != -1 || errno != EFAULT) {
        fprintf(stderr, "present-fault: ioctl GFX_PRESENT expected -1 EFAULT, got %d errno=%d\n",
                prc, errno);
        gfx_close(gh);
        return 1;
    }

    /* Close the handle: must release lease cleanly and detach */
    gfx_close(gh);

    /* Open new view, acquire and present to verify coordinator is fully functional */
    gfx_handle_t *gh2 = gfx_open(0, 0, w, h);
    if (!gh2) {
        fprintf(stderr, "present-fault: subsequent gfx_open failed (errno=%d)\n", errno);
        return 1;
    }
    if (gfx_acquire(gh2) != 0) {
        fprintf(stderr, "present-fault: subsequent gfx_acquire failed (errno=%d)\n", errno);
        gfx_close(gh2);
        return 1;
    }
    if (gfx_present(gh2) != 0) {
        fprintf(stderr, "present-fault: subsequent gfx_present failed (errno=%d)\n", errno);
        gfx_close(gh2);
        return 1;
    }
    gfx_release(gh2);
    gfx_close(gh2);

    printf("[TEST_DISPLAY_OWNER] present-fault PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        /* Run all self-contained tests */
        if (cmd_kill_owner() != 0) return 1;
        if (cmd_dup_close() != 0) return 1;
        if (cmd_fork_close() != 0) return 1;
        if (cmd_close_before_exec() != 0) return 1;
        if (cmd_readonly_write() != 0) return 1;
        if (cmd_readonly_mprotect() != 0) return 1;
        if (cmd_present_fault() != 0) return 1;
        printf("[TEST_DISPLAY_OWNER] ALL PASS\n");
        return 0;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "hold") == 0) {
        return cmd_hold();
    } else if (strcmp(sub, "kill-owner") == 0) {
        return cmd_kill_owner();
    } else if (strcmp(sub, "dup-close") == 0) {
        return cmd_dup_close();
    } else if (strcmp(sub, "fork-close") == 0) {
        return cmd_fork_close();
    } else if (strcmp(sub, "close-before-exec") == 0) {
        return cmd_close_before_exec();
    } else if (strcmp(sub, "readonly-write") == 0) {
        return cmd_readonly_write();
    } else if (strcmp(sub, "readonly-mprotect") == 0) {
        return cmd_readonly_mprotect();
    } else if (strcmp(sub, "present-fault") == 0) {
        return cmd_present_fault();
    } else if (strcmp(sub, "exec-target") == 0) {
        return 0;
    } else if (strcmp(sub, "all") == 0) {
        if (cmd_kill_owner() != 0) return 1;
        if (cmd_dup_close() != 0) return 1;
        if (cmd_fork_close() != 0) return 1;
        if (cmd_close_before_exec() != 0) return 1;
        if (cmd_readonly_write() != 0) return 1;
        if (cmd_readonly_mprotect() != 0) return 1;
        if (cmd_present_fault() != 0) return 1;
        printf("[TEST_DISPLAY_OWNER] ALL PASS\n");
        return 0;
    }

    fprintf(stderr, "unknown subcommand: %s\n", sub);
    return 1;
}
