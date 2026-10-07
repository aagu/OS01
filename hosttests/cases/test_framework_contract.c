/* test_framework_contract.c — negative contract binary for the test
 * framework itself (test-framework plan Task 1).
 *
 * This binary is deliberately NOT part of TEST_BINS: it intentionally
 * fails and must exit nonzero. It pins three framework contracts:
 *
 *   1. A failing assertion propagates to the process exit status
 *      (main returns __test_stats.failed's verdict).
 *   2. Assertion macro arguments are evaluated exactly ONCE, even on
 *      the failure-print path (the old macro re-evaluated them inside
 *      the printf, doubling side effects).
 *   3. TEST_RESULTS() prints a snapshot without resetting __test_stats;
 *      failure totals survive two consecutive summaries.
 *
 * Exit codes:
 *   1 — contract holds: failures recorded, totals intact (EXPECTED)
 *   2 — macro double-evaluation detected (assert_* argument bug)
 *   3 — TEST_RESULTS() reset the stats (old implicit-reset bug)
 */

#include <test_framework.h>

static int g_eval_count = 0;
static int g_str_eval_count = 0;
static int g_mem_eval_count = 0;

/* Always returns 0; counts how many times the assertion macro actually
 * evaluated its argument. */
static int counting_zero(void)
{
    g_eval_count++;
    return 0;
}

/* Same probe for the char*-valued assertion. */
static const char *counting_str(void)
{
    g_str_eval_count++;
    return "abc";
}

/* Same probe for the size-valued assertion. */
static size_t counting_size(void)
{
    g_mem_eval_count++;
    return 4;
}

int main(void)
{
    TEST_SUITE("framework-contract");

    /* The deliberate failures. Each one fails AND, because the
     * failure-print path formats the operand, doubles as the
     * single-evaluation probe for its macro. */
    assert_eq(counting_zero(), 123);
    assert_str_eq(counting_str(), "xyz");
    assert_mem_eq("aaaa", "bbbb", counting_size());

    if (g_eval_count != 1 || g_str_eval_count != 1 || g_mem_eval_count != 1) {
        printf("  [CONTRACT] assertion argument evaluated more than once: "
               "eq=%d str_eq=%d mem_eq=%d (all expected 1)\n",
               g_eval_count, g_str_eval_count, g_mem_eval_count);
        return 2;
    }

    /* First summary: must print "Failed: 3" and NOT clear the stats. */
    TEST_RESULTS();

    if (__test_stats.total != 3 || __test_stats.failed != 3) {
        printf("  [CONTRACT] TEST_RESULTS() reset stats: "
               "total=%d failed=%d (expected total=3 failed=3)\n",
               __test_stats.total, __test_stats.failed);
        return 3;
    }

    /* Second summary must report the same totals as the first. */
    TEST_RESULTS();

    /* Contract binary is green exactly when the framework is broken. */
    return __test_stats.failed > 0 ? 1 : 0;
}
