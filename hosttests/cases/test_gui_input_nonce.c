/*
 * test_gui_input_nonce.c — the GUI-2 input probe's nonce rule
 *
 * GUI-2 plan Task 10 (fix round 1).  The probe's CLI is `--nonce <hex>`:
 * the nonce is interpolated into the control file's path
 * (/tmp/gui-input-<hex>.ctl) and the probe unlinks that path when it
 * exits.  A nonce that is not hex could therefore escape /tmp.
 *
 * This fixture host-compiles the REAL rule (the header the guest probe
 * uses) and pins:
 *   - a valid nonce is a non-empty hex string of at most
 *     GUI_INPUT_NONCE_MAX_CHARS characters;
 *   - everything else — an empty string, a path, a shell fragment, a
 *     whitespace, an over-long string — is rejected;
 *   - the control path is exactly /tmp/gui-input-<hex>.ctl and can only
 *     ever live in /tmp;
 *   - a rejected nonce never produces a path (the output stays empty);
 *   - a NULL or a zero-length output buffer is rejected, and nothing is
 *     written to it.
 */
#include <test_framework.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The REAL production rule, shared with the guest probe. */
#include <gui_input_nonce.h>

/* The nonces the probe must accept. */
static const char *const k_valid[] = {
    "0",
    "a",
    "f",
    "ab12",
    "ABCDEF",
    "0123456789abcdef",
    "a1b2c3d4e5f60718",
    "a1b2c3d4e5f60718a1b2c3d4e5f60718",   /* exactly the maximum */
};

/* The nonces the probe must reject. */
static const char *const k_invalid[] = {
    "",
    "zz",
    "not-hex",
    "../../etc/shadow",
    "a b",
    "x;rm -rf /",
    "a/b",
    "a.b",
    "0x1234",
    "dead beef",
    "a\nb",
    "a\tb",
    "-1",
    "+1",
    "a1b2c3d4e5f60718a1b2c3d4e5f607189",   /* one over the maximum */
};

static void test_valid_nonces_are_accepted(void)
{
    size_t i;

    for (i = 0; i < sizeof(k_valid) / sizeof(k_valid[0]); i++) {
        assert_true(gui_input_nonce_is_valid(k_valid[i]));
    }
}

static void test_invalid_nonces_are_rejected(void)
{
    size_t i;

    for (i = 0; i < sizeof(k_invalid) / sizeof(k_invalid[0]); i++) {
        assert_false(gui_input_nonce_is_valid(k_invalid[i]));
    }
}

static void test_null_nonce_is_rejected(void)
{
    assert_false(gui_input_nonce_is_valid(NULL));
}

static void test_the_maximum_length_is_inclusive(void)
{
    char max[GUI_INPUT_NONCE_MAX_CHARS + 2];

    memset(max, 'a', sizeof(max) - 1u);
    max[sizeof(max) - 1u] = '\0';
    max[GUI_INPUT_NONCE_MAX_CHARS] = '\0';
    assert_true(gui_input_nonce_is_valid(max));

    max[GUI_INPUT_NONCE_MAX_CHARS] = 'a';
    max[GUI_INPUT_NONCE_MAX_CHARS + 1u] = '\0';
    assert_false(gui_input_nonce_is_valid(max));
}

static void test_the_control_path_is_exactly_the_fixed_shape(void)
{
    char path[128];
    size_t slashes = 0;
    size_t i;

    assert_eq(0, gui_input_nonce_ctl_path(path, sizeof(path), "a1b2c3d4"));
    assert_str_eq("/tmp/gui-input-a1b2c3d4.ctl", path);

    /* The path can only ever live in /tmp: exactly two separators, both
     * in the leading "/tmp/" and in the ".ctl" suffix's absence. */
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            slashes++;
            assert_true(i < 6u);      /* only the leading "/tmp/" */
        }
    }
    assert_eq(2, slashes);
    assert_true(strstr(path, "..") == NULL);
}

static void test_a_rejected_nonce_never_builds_a_path(void)
{
    char path[128];
    size_t i;

    for (i = 0; i < sizeof(k_invalid) / sizeof(k_invalid[0]); i++) {
        memset(path, 'X', sizeof(path));
        assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), k_invalid[i]));
        /* The output is emptied, so a caller that ignores the result
         * still cannot build a path outside /tmp. */
        assert_str_eq("", path);
    }
}

static void test_a_too_small_buffer_is_rejected(void)
{
    char path[8];

    memset(path, 'X', sizeof(path));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "a1b2c3d4"));
    assert_str_eq("", path);
}

static void test_a_null_output_buffer_is_rejected(void)
{
    /* The output buffer is mandatory.  Without the guard the builder
     * would dereference NULL before it could reject anything. */
    assert_eq(-1, gui_input_nonce_ctl_path(NULL, 128u, "a1b2c3d4"));
}

static void test_a_zero_length_output_buffer_is_rejected(void)
{
    char path[128];

    memset(path, 'X', sizeof(path));
    assert_eq(-1, gui_input_nonce_ctl_path(path, 0u, "a1b2c3d4"));
    /* Nothing was written: a zero-length buffer is rejected before the
     * builder touches the output. */
    assert_eq('X', path[0]);
    assert_eq('X', path[1]);
    assert_eq('X', path[sizeof(path) - 1u]);
}

static void test_a_path_escape_cannot_be_built(void)
{
    char path[128];

    /* The classic escapes are rejected by construction. */
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "../../etc/shadow"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "../x"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "/etc/shadow"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "a/../b"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "a b"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "a;rm -rf /"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "`id`"));
    assert_eq(-1, gui_input_nonce_ctl_path(path, sizeof(path), "$(id)"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_valid_nonces_are_accepted),
    TEST_ENTRY(test_invalid_nonces_are_rejected),
    TEST_ENTRY(test_null_nonce_is_rejected),
    TEST_ENTRY(test_the_maximum_length_is_inclusive),
    TEST_ENTRY(test_the_control_path_is_exactly_the_fixed_shape),
    TEST_ENTRY(test_a_rejected_nonce_never_builds_a_path),
    TEST_ENTRY(test_a_too_small_buffer_is_rejected),
    TEST_ENTRY(test_a_null_output_buffer_is_rejected),
    TEST_ENTRY(test_a_zero_length_output_buffer_is_rejected),
    TEST_ENTRY(test_a_path_escape_cannot_be_built),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
