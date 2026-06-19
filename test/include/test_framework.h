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
    __test_stats.total++; \
    if ((long)(expected) != (long)(actual)) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_eq(" #expected "=%ld, " #actual "=%ld)\n", \
               __FILE__, __LINE__, (long)(expected), (long)(actual)); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

#define assert_str_eq(expected, actual) do { \
    __test_stats.total++; \
    if (strcmp((expected), (actual)) != 0) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_str_eq(expected=\"%s\", actual=\"%s\")\n", \
               __FILE__, __LINE__, (expected), (actual)); \
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
    __test_stats.total++; \
    if (memcmp((expected), (actual), (size)) != 0) { \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: assert_mem_eq(%zu bytes)\n", \
               __FILE__, __LINE__, (size_t)(size)); \
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

#define RUN_ALL_TESTS() \
    do { \
        printf("=== Test Runner ===\n"); \
        (void)__test_table_size; \
        int __table_size = sizeof(__test_table) / sizeof(__test_table[0]); \
        for (int __i = 0; __i < __table_size; __i++) { \
            printf("\n--- %s ---\n", __test_table[__i].name); \
            __test_table[__i].fn(); \
        } \
        TEST_RESULTS(); \
    } while(0)

#endif /* TEST_FRAMEWORK_H */
