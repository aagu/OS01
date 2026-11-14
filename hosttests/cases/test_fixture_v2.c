/* @os01-test
 * id: test_fixture_v2
 * layer: host
 * subsystem: framework
 * timing: strict
 * tier: pr
 * isolation: fork
 * scenarios: [framework.fixture.v2]
 */
#include <test_framework.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static char *g_shared_order = NULL;

static void append_order(const char *tag)
{
    if (g_shared_order) {
        strcat(g_shared_order, tag);
    }
}

static void fixture_setup(void)
{
    append_order("S");
}

static void fixture_teardown(void)
{
    append_order("T");
}

TEST_FUNC(test_setup_teardown_runs_body)
{
    assert_str_eq("S", g_shared_order);
    append_order("B");
}

TEST_FUNC(test_setup_teardown_order_verified)
{
    assert_str_eq("SBT", g_shared_order);
}

/* Sub-suite helpers for testing fork segfault isolation and %-safe output */
static void segfault_case(void)
{
    volatile int *p = (volatile int *)0;
    *p = 42;
}

static void survivor_case(void)
{
    assert_true(1);
}

static int run_segfault_subsuite(void)
{
    TEST_RESET();
    test_entry_t __test_table[] = {
        TEST_ENTRY(segfault_case),
        TEST_ENTRY(survivor_case),
    };
    int __test_table_size = (int)(sizeof(__test_table) / sizeof(__test_table[0]));
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

static void read_all_fd(int fd, char *buf, size_t cap)
{
    size_t pos = 0;
    while (pos + 1 < cap) {
        ssize_t n = read(fd, buf + pos, cap - 1 - pos);
        if (n <= 0) {
            break;
        }
        pos += (size_t)n;
    }
    buf[pos] = '\0';
}

TEST_FUNC(test_fork_segfault_isolation)
{
    int pipefd[2];
    assert_eq(0, pipe(pipefd));
    fflush(stdout);
    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int rc = run_segfault_subsuite();
        fflush(stdout);
        _exit(rc);
    }
    close(pipefd[1]);
    char out[4096];
    read_all_fd(pipefd[0], out, sizeof(out));
    close(pipefd[0]);

    int status = 0;
    assert_eq(pid, waitpid(pid, &status, 0));
    assert_true(WIFEXITED(status));
    assert_eq(1, WEXITSTATUS(status));
    assert_not_null(strstr(out, "[TEST] FAIL segfault_case reason=signal_11"));
    assert_not_null(strstr(out, "[TEST] PASS survivor_case"));
    assert_not_null(strstr(out, "[TEST] END suite=hosttests total=2 passed=1 failed=1 skipped=0"));
}

TEST_FUNC(test_percent_safe_assertion_output)
{
    int pipefd[2];
    assert_eq(0, pipe(pipefd));
    fflush(stdout);
    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        TEST_RESET();
        assert_eq(100 % 7, 3);
        fflush(stdout);
        _exit(__test_stats.failed == 1 ? 0 : 1);
    }
    close(pipefd[1]);
    char out[2048];
    read_all_fd(pipefd[0], out, sizeof(out));
    close(pipefd[0]);

    int status = 0;
    assert_eq(pid, waitpid(pid, &status, 0));
    assert_true(WIFEXITED(status));
    assert_eq(0, WEXITSTATUS(status));
    assert_not_null(strstr(out, "assert_eq(100 % 7=2, 3=3)"));
}

TEST_LIST_BEGIN
    TEST_ENTRY_EX(test_setup_teardown_runs_body, fixture_setup, fixture_teardown),
    TEST_ENTRY_EX(test_setup_teardown_order_verified, NULL, NULL),
    TEST_ENTRY(test_fork_segfault_isolation),
    TEST_ENTRY(test_percent_safe_assertion_output),
TEST_LIST_END

int main(void)
{
    g_shared_order = mmap(NULL, 256, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shared_order == MAP_FAILED) {
        return 2;
    }
    g_shared_order[0] = '\0';

    TEST_SUITE("fixture-v2");
    RUN_ALL_TESTS();
    int rc = __test_stats.failed > 0 ? 1 : 0;
    munmap(g_shared_order, 256);
    return rc;
}
