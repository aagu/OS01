#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Test result tracking ──────────────────────────── */
typedef struct {
    int total;
    int passed;
    int failed;
    int skipped;
    const char *current_suite;
} test_stats_t;

static test_stats_t __test_stats;

#define TEST_SUITE(name) \
    do { \
        __test_stats.current_suite = name; \
        printf("\n  == %s ==\n", name); \
    } while(0)

/* ── Core assertions ───────────────────────────────── */
#define assert_true(cond) do { \
    __test_stats.total++; \
    if (!(cond)) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_true(%s)\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

#define assert_false(cond) assert_true(!(cond))

#define assert_eq(expected, actual) do { \
    long __e = (long)(expected); \
    long __a = (long)(actual); \
    __test_stats.total++; \
    if (__e != __a) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_eq(" #expected "=%ld, " #actual "=%ld)\n", \
               __FILE__, __LINE__, __e, __a); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

#define assert_str_eq(expected, actual) do { \
    const char *__se = (expected); \
    const char *__sa = (actual); \
    __test_stats.total++; \
    if (strcmp(__se, __sa) != 0) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_str_eq(expected=\"%s\", actual=\"%s\")\n", \
               __FILE__, __LINE__, __se, __sa); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

#define assert_null(ptr) do { \
    __test_stats.total++; \
    if ((ptr) != NULL) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_null(%s) is not NULL\n", \
               __FILE__, __LINE__, #ptr); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

#define assert_not_null(ptr) do { \
    __test_stats.total++; \
    if ((ptr) == NULL) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_not_null(%s) is NULL\n", \
               __FILE__, __LINE__, #ptr); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

#define assert_mem_eq(expected, actual, size) do { \
    size_t __msz = (size_t)(size); \
    __test_stats.total++; \
    if (memcmp((expected), (actual), __msz) != 0) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_mem_eq(%zu bytes)\n", \
               __FILE__, __LINE__, __msz); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

/* ── Test result reporting ─────────────────────────── */
#define TEST_RESULTS() \
    do { \
        int __total = __test_stats.total; \
        int __passed = __test_stats.passed; \
        int __failed = __test_stats.failed; \
        printf("\n  ---\n"); \
        printf("  Total: %d | Passed: %d | Failed: %d\n", \
               __total, __passed, __failed); \
        if (__failed > 0) { \
            printf("  >>> SOME TESTS FAILED <<<\n"); \
        } else { \
            printf("  >>> ALL TESTS PASSED <<<\n"); \
        } \
    } while(0)

#define TEST_RESET() \
    do { \
        __test_stats.total = 0; \
        __test_stats.passed = 0; \
        __test_stats.failed = 0; \
        __test_stats.skipped = 0; \
    } while(0)

/* ── Test registration ────────────────────────────── */
#define TEST_FUNC(name) void name(void)

typedef struct {
    const char *name;
    void (*fn)(void);
} test_entry_t;

#define TEST_LIST_BEGIN \
    static test_entry_t __test_table[] = {

#define TEST_ENTRY(fn) { #fn, fn }

#define TEST_LIST_END \
    }; \
    static int __test_table_size = sizeof(__test_table) / sizeof(__test_table[0]);

/* ── Protocol v1 emission (test-framework plan, Task 7) ─────────────
 * RUN_ALL_TESTS() publishes a machine-readable protocol-v1 trace so the
 * host runner (qemutests/run_hosttests.py) can validate *case* identity
 * and counts instead of trusting the process exit status alone.
 *
 * Assertion counts stay separate from case counts: TEST_RESULTS() keeps
 * printing the assertion summary ("Total/Passed/Failed" counts every
 * assert_* macro), while the [TEST] END line reports CASE counts (one per
 * registered test function).  A case is FAIL when the assertion-failure
 * counter advanced across its body.
 *
 * The suite id is a single fixed label for the host framework; each
 * binary is an independent unit for the runner (which archives one report
 * per binary).  Case ids are the registered test-function names, which are
 * C identifiers and therefore already match the protocol's
 * [A-Za-z0-9_.-]+ grammar.
 *
 * The host registry has no per-case "optional" flag, so every default
 * case is declared required=1 (spec 6.1: default-selected ordinary cases
 * are mandatory).  No SKIP record is ever emitted from this path. */
#define HOSTTEST_SUITE_ID "hosttests"

#define RUN_ALL_TESTS() \
    do { \
        printf("=== Test Runner ===\n"); \
        (void)__test_table_size; \
        int __table_size = (int)(sizeof(__test_table) / sizeof(__test_table[0])); \
        int __case_passed = 0; \
        int __case_failed = 0; \
        printf("[TEST] START v=1 suite=%s expected=%d\n", \
               HOSTTEST_SUITE_ID, __table_size); \
        for (int __s = 0; __s < __table_size; __s++) { \
            printf("[TEST] SELECT %s required=1\n", __test_table[__s].name); \
        } \
        for (int __i = 0; __i < __table_size; __i++) { \
            printf("\n--- %s ---\n", __test_table[__i].name); \
            int __before_failed = __test_stats.failed; \
            __test_table[__i].fn(); \
            int __case_delta = __test_stats.failed - __before_failed; \
            printf("[TEST] BEGIN %s\n", __test_table[__i].name); \
            if (__case_delta == 0) { \
                __case_passed++; \
                printf("[TEST] PASS %s\n", __test_table[__i].name); \
            } else { \
                __case_failed++; \
                printf("[TEST] FAIL %s reason=assertion_failures_%d\n", \
                       __test_table[__i].name, __case_delta); \
            } \
        } \
        TEST_RESULTS(); \
        printf("[TEST] END suite=%s total=%d passed=%d failed=%d skipped=%d\n", \
               HOSTTEST_SUITE_ID, __table_size, __case_passed, __case_failed, 0); \
    } while(0)

#endif /* TEST_FRAMEWORK_H */
