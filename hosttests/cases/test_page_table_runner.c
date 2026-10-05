/* A failed assertion must survive TEST_RESULTS and fail the process. */
#include "page_table_test_runner.h"
#include <sys/wait.h>
#include <unistd.h>
static void intentional_failure(void) { assert_true(0); }
int main(void)
{
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        test_entry_t tests[] = {{"intentional failure", intentional_failure}};
        _exit(page_table_run_tests(tests, 1));
    }
    int status;
    if (waitpid(child, &status, 0) != child) return 2;
    return !WIFEXITED(status) || WEXITSTATUS(status) != 1;
}
