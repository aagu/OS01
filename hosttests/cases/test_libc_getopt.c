/*
 * test/cases/test_libc_getopt.c — getopt option parsing tests
 *
 * These exercise the CURRENT (buggy) libc getopt implementation. They are
 * expected to FAIL until the getopt fix lands in a later task. No libc source
 * is modified here.
 */
#include "test_framework.h"
#include <getopt.h>
#include <string.h>

/* Reset global parser state before each scenario. */
static void reset_getopt(void) {
    optind = 1;
    optarg = NULL;
    optopt = 0;
    opterr = 1;
}

TEST_FUNC(test_getopt_bundled) {
    reset_getopt();
    char *argv[] = {"prog", "-abc", NULL};
    int argc = 2;
    int r;
    r = getopt(argc, argv, "abc");
    assert_eq(r, 'a');
    r = getopt(argc, argv, "abc");
    assert_eq(r, 'b');
    r = getopt(argc, argv, "abc");
    assert_eq(r, 'c');
    r = getopt(argc, argv, "abc");
    assert_eq(r, -1);
}

TEST_FUNC(test_getopt_attached_value) {
    reset_getopt();
    char *argv[] = {"prog", "-dVAL", NULL};
    int argc = 2;
    int r = getopt(argc, argv, "d:");
    assert_eq(r, 'd');
    /* buggy: optarg points at the whole "-dVAL" instead of "VAL" */
    assert_str_eq(optarg, "VAL");
}

TEST_FUNC(test_getopt_separate_value) {
    reset_getopt();
    char *argv[] = {"prog", "-d", "VAL", NULL};
    int argc = 3;
    int r = getopt(argc, argv, "d:");
    assert_eq(r, 'd');
    assert_str_eq(optarg, "VAL");
}

TEST_FUNC(test_getopt_missing_arg_colon) {
    reset_getopt();
    char *argv[] = {"prog", "-a", NULL};
    int argc = 2;
    /* optstring begins with ':' => missing arg should return ':' */
    int r = getopt(argc, argv, ":a:");
    assert_eq(r, ':');
}

TEST_FUNC(test_getopt_dash_mode) {
    reset_getopt();
    char *argv[] = {"prog", "file1", "file2", NULL};
    int argc = 3;
    /* optstring begins with '-' => operands returned as code 1 with optarg */
    int r = getopt(argc, argv, "-ab");
    assert_eq(r, 1);
    if (r == 1)
        assert_str_eq(optarg, "file1");
    r = getopt(argc, argv, "-ab");
    assert_eq(r, 1);
    if (r == 1)
        assert_str_eq(optarg, "file2");
}

TEST_FUNC(test_getopt_double_dash) {
    reset_getopt();
    char *argv[] = {"prog", "--", "-a", NULL};
    int argc = 3;
    int r = getopt(argc, argv, "a");
    assert_eq(r, -1);
}

TEST_FUNC(test_getopt_plus_stop) {
    reset_getopt();
    char *argv[] = {"prog", "-a", "file", "-b", NULL};
    int argc = 4;
    int r;
    /* optstring begins with '+' => POSIX + mode:
     * stop scanning at the first non-option, no permutation. */
    r = getopt(argc, argv, "+ab");
    assert_eq(r, 'a');
    r = getopt(argc, argv, "+ab");
    assert_eq(r, -1);
    /* must stop at "file" (index 2), leaving "-b" unprocessed */
    assert_eq(optind, 2);

    /* In + mode the leading '+' is a mode flag, NOT an option char.
     * A "-+" token must be reported as an unknown option ('?'),
     * not matched as option '+'. */
    reset_getopt();
    char *argv2[] = {"prog", "-+", NULL};
    int argc2 = 2;
    r = getopt(argc2, argv2, "+ab");
    assert_eq(r, '?');
}

TEST_FUNC(test_getopt_reset) {
    reset_getopt();
    char *argv[] = {"prog", "-a", "-b", NULL};
    int argc = 3;
    int r = getopt(argc, argv, "ab");
    assert_eq(r, 'a');
    r = getopt(argc, argv, "ab");
    assert_eq(r, 'b');
    r = getopt(argc, argv, "ab");
    assert_eq(r, -1);
    /* optind=0 must re-initialize the scan from the start */
    optind = 0;
    r = getopt(argc, argv, "ab");
    assert_eq(r, 'a');
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_getopt_bundled),
    TEST_ENTRY(test_getopt_attached_value),
    TEST_ENTRY(test_getopt_separate_value),
    TEST_ENTRY(test_getopt_missing_arg_colon),
    TEST_ENTRY(test_getopt_dash_mode),
    TEST_ENTRY(test_getopt_double_dash),
    TEST_ENTRY(test_getopt_plus_stop),
    TEST_ENTRY(test_getopt_reset),
TEST_LIST_END

int main() {
    printf("=== Test Runner ===\n");
    int size = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int i = 0; i < size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }
    int failed = __test_stats.failed;
    printf("\n  ---\n");
    printf("  Total: %d | Passed: %d | Failed: %d\n",
           __test_stats.total, __test_stats.passed, failed);
    if (failed > 0)
        printf("  >>> SOME TESTS FAILED <<<\n");
    else
        printf("  >>> ALL TESTS PASSED <<<\n");
    return failed > 0 ? 1 : 0;
}
