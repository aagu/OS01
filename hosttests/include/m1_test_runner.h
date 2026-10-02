#ifndef M1_TEST_RUNNER_H
#define M1_TEST_RUNNER_H
#include "test_framework.h"

/* TEST_RESULTS resets counters; preserve failure status for the process. */
static int m1_run_tests(test_entry_t *tests, size_t count)
{
    printf("=== Test Runner ===\n");
    for (size_t i = 0; i < count; ++i) {
        printf("\n--- %s ---\n", tests[i].name);
        tests[i].fn();
    }
    int failed = __test_stats.failed != 0;
    TEST_RESULTS();
    return failed;
}
#define M1_RUN_ALL_TESTS() m1_run_tests(__test_table, (size_t)__test_table_size)
#endif
