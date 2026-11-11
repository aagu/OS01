/*
 * test_doom_diag.c — Doom diagnostic logging & state observation host test
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_framework.h"
#include "diag.h"

/* Define dummy key variables for host test linkage */
int key_fire = 157;
int key_use = 57;
int key_speed = 182;
int key_strafe = 184;
int key_right = 205;
int key_left = 203;
int key_up = 200;
int key_down = 208;

static int pipe_fds[2];

static void setup_pipe(void)
{
    if (pipe(pipe_fds) < 0) {
        perror("pipe");
        exit(1);
    }
    doom_diag_set_output_fd(pipe_fds[1]);
}

static void teardown_pipe(void)
{
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    doom_diag_set_output_fd(-1);
}

static void read_line(char *buf, size_t max_len)
{
    size_t idx = 0;
    while (idx + 1 < max_len) {
        char ch;
        ssize_t n = read(pipe_fds[0], &ch, 1);
        if (n <= 0) break;
        buf[idx++] = ch;
        if (ch == '\n') break;
    }
    buf[idx] = '\0';
}

static void test_diag_emit_and_sequence(void)
{
    doom_diag_init("testnonce123");
    char line[512];

    doom_diag_emit("test", "key=val");
    read_line(line, sizeof(line));

    assert_true(strstr(line, "[doom-diag] v=1") != NULL);
    assert_true(strstr(line, "nonce=testnonce123") != NULL);
    assert_true(strstr(line, "seq=1") != NULL);
    assert_true(strstr(line, "type=test") != NULL);
    assert_true(strstr(line, "key=val\n") != NULL);

    /* Second emit must have seq=2 */
    doom_diag_emit("test2", "a=b");
    read_line(line, sizeof(line));
    assert_true(strstr(line, "seq=2") != NULL);
    assert_true(strstr(line, "type=test2") != NULL);
}

static void test_diag_first_frame(void)
{
    doom_diag_init("ffnonce");
    char line[512];

    assert_eq(0, doom_diag_first_frame_done());
    doom_diag_first_frame();
    assert_eq(1, doom_diag_first_frame_done());

    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=ready") != NULL);
    assert_true(strstr(line, "frames=1") != NULL);

    /* Subsequent first_frame calls must be idempotent (no duplicate ready emit) */
    doom_diag_first_frame();
    assert_eq(1, doom_diag_first_frame_done());
}

static void test_diag_tic(void)
{
    doom_diag_init("ticnonce");
    char line[512];

    /* Tic with forwardmove=25 */
    doom_diag_tic(10, 5, 1, 1, 100, 200, 3000, 25, 0, 0, 0);
    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=tic") != NULL);
    assert_true(strstr(line, "gametic=10") != NULL);
    assert_true(strstr(line, "fwd=25") != NULL);

    /* Key release (forwardmove=0) must still emit to prove UP zeroing */
    doom_diag_tic(11, 6, 1, 1, 105, 200, 3000, 0, 0, 0, 0);
    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=tic") != NULL);
    assert_true(strstr(line, "gametic=11") != NULL);
    assert_true(strstr(line, "fwd=0") != NULL);
}

static void test_diag_save_and_load(void)
{
    doom_diag_init("saveload");
    char line[512];

    doom_diag_save(0, "slot0name", 1, 50, 60, 1000, 100);
    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=save") != NULL);
    assert_true(strstr(line, "slot=0") != NULL);
    assert_true(strstr(line, "desc=slot0name") != NULL);
    assert_true(strstr(line, "hp=100") != NULL);

    doom_diag_load(0, 1, 50, 60, 1000, 100);
    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=load") != NULL);
    assert_true(strstr(line, "slot=0") != NULL);
    assert_true(strstr(line, "hp=100") != NULL);
}

static void test_diag_bindings(void)
{
    doom_diag_init("bindnonce");
    char line[512];

    doom_diag_bindings();
    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=bindings") != NULL);
    assert_true(strstr(line, "fire=157") != NULL);
    assert_true(strstr(line, "use=57") != NULL);
    assert_true(strstr(line, "speed=182") != NULL);
}

static void test_diag_summary(void)
{
    doom_diag_init("summnonce");
    char line[512];

    doom_diag_present_stat(0, 0, 0);
    /* first present emits ready */
    read_line(line, sizeof(line));

    doom_diag_present_stat(1, EAGAIN, 0);
    doom_diag_present_stat(-1, EIO, 0);
    doom_diag_summary();
    read_line(line, sizeof(line));
    assert_true(strstr(line, "type=summary") != NULL);
    assert_true(strstr(line, "frames=1") != NULL);
    assert_true(strstr(line, "eagain=1") != NULL);
    assert_true(strstr(line, "errs=1") != NULL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_diag_emit_and_sequence),
    TEST_ENTRY(test_diag_first_frame),
    TEST_ENTRY(test_diag_tic),
    TEST_ENTRY(test_diag_save_and_load),
    TEST_ENTRY(test_diag_bindings),
    TEST_ENTRY(test_diag_summary),
TEST_LIST_END

int main(void)
{
    setup_pipe();
    RUN_ALL_TESTS();
    teardown_pipe();
    return __test_stats.failed > 0 ? 1 : 0;
}
