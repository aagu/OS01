#ifndef PAGE_TABLE_TEST_RUNNER_H
#define PAGE_TABLE_TEST_RUNNER_H
#include "test_framework.h"

/* TEST_RESULTS prints a snapshot without resetting __test_stats, so
 * main reads the surviving failure count for the process exit status. */
static int page_table_run_tests(test_entry_t *tests, size_t count)
{
    printf("=== Test Runner ===\n");
    for (size_t i = 0; i < count; ++i) {
        printf("\n--- %s ---\n", tests[i].name);
        tests[i].fn();
    }
    TEST_RESULTS();
    return __test_stats.failed != 0;
}
#define PAGE_TABLE_RUN_ALL_TESTS() page_table_run_tests(__test_table, (size_t)__test_table_size)
#endif
