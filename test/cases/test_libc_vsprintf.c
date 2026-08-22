/*
 * test/cases/test_libc_vsprintf.c — vsprintf format string tests
 */
#include "test_framework.h"
#include <string.h>
#include <stdarg.h>

extern int vsprintf(char *buf, const char *fmt, va_list args);
extern int sprintf(char *buf, const char *fmt, ...);
extern int snprintf(char *buf, unsigned long size, const char *fmt, ...);
extern int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list args);

static int my_sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    int ret;
    va_start(args, fmt);
    ret = vsprintf(buf, fmt, args);
    va_end(args);
    return ret;
}

static int my_vsnprintf(char *buf, unsigned long size, const char *fmt, ...) {
    va_list args;
    int ret;
    va_start(args, fmt);
    ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

TEST_FUNC(test_sprintf_strings) {
    char buf[256];
    int n = my_sprintf(buf, "hello %s", "world");
    assert_str_eq(buf, "hello world");
    assert_eq(n, 11);
}

TEST_FUNC(test_sprintf_integers) {
    char buf[256];
    my_sprintf(buf, "%d", 42);
    assert_str_eq(buf, "42");
    my_sprintf(buf, "%d", -42);
    assert_str_eq(buf, "-42");
}

TEST_FUNC(test_sprintf_hex) {
    char buf[256];
    my_sprintf(buf, "%x", 255);
    assert_str_eq(buf, "ff");
    my_sprintf(buf, "%X", 255);
    assert_str_eq(buf, "FF");
}

TEST_FUNC(test_sprintf_multiple) {
    char buf[256];
    my_sprintf(buf, "%s=%d", "count", 10);
    assert_str_eq(buf, "count=10");
}

TEST_FUNC(test_sprintf_char) {
    char buf[256];
    my_sprintf(buf, "char: %c", 'A');
    assert_str_eq(buf, "char: A");
}

TEST_FUNC(test_sprintf_ptr) {
    char buf[256];
    int n = my_sprintf(buf, "%p", (void*)0x1234);
    assert_true(n > 0);
    assert_true(buf[0] != 0);
    // %p should produce hex output containing '1234'
    assert_true(strstr(buf, "1234") != NULL);
}

TEST_FUNC(test_sprintf_percent) {
    char buf[256];
    my_sprintf(buf, "100%%");
    assert_str_eq(buf, "100%");
}

TEST_FUNC(test_sprintf_unsigned) {
    char buf[256];
    my_sprintf(buf, "%u", 42u);
    assert_str_eq(buf, "42");
    my_sprintf(buf, "%lu", 42UL);
    assert_str_eq(buf, "42");
    my_sprintf(buf, "%llu", 123456789012345ULL);
    assert_str_eq(buf, "123456789012345");
}

TEST_FUNC(test_sprintf_longlong) {
    char buf[256];
    /* Current vsprintf only consumes a single 'l' qualifier, so "%llu"/"%lld"
     * fall through to the default handler and print the literal "%lu"/"%lld". */
    my_sprintf(buf, "%llu", 123456789012345ULL);
    assert_str_eq(buf, "123456789012345");
    my_sprintf(buf, "%lld", -123456789012345LL);
    assert_str_eq(buf, "-123456789012345");
    my_sprintf(buf, "%llu", 0ULL);
    assert_str_eq(buf, "0");
}

TEST_FUNC(test_snprintf_truncation) {
    char buf[16];
    int n = -1;
    int r1 = snprintf(buf, 3, "ab%ncd", &n);
    assert_eq(r1, 4);
    assert_eq(n, 2);
    assert_str_eq(buf, "ab");

    int late_n = -1;
    int r2 = snprintf(buf, 3, "abcd%nef", &late_n);
    assert_eq(r2, 6);
    assert_eq(late_n, 4);
    assert_str_eq(buf, "ab");
}

TEST_FUNC(test_snprintf_truncation_value) {
    char buf[16];
    /* snprintf must return the full would-be length, not size-1.
     * With size==1 only the NUL fits (cap-1 guard), so buf is empty. */
    assert_eq(snprintf(buf, 1, "abcd"), 4);
    assert_str_eq(buf, "");
}

TEST_FUNC(test_snprintf_null_zero) {
    /* snprintf(NULL, 0, ...) must return the would-be length */
    assert_eq(snprintf(NULL, 0, "hello"), 5);
}

TEST_FUNC(test_vsnprintf_basic) {
    char buf[16];
    int ret = my_vsnprintf(buf, sizeof(buf), "val=%d", 7);
    assert_eq(ret, 5);
    assert_str_eq(buf, "val=7");
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_sprintf_strings),
    TEST_ENTRY(test_sprintf_integers),
    TEST_ENTRY(test_sprintf_hex),
    TEST_ENTRY(test_sprintf_multiple),
    TEST_ENTRY(test_sprintf_char),
    TEST_ENTRY(test_sprintf_ptr),
    TEST_ENTRY(test_sprintf_percent),
    TEST_ENTRY(test_sprintf_unsigned),
    TEST_ENTRY(test_sprintf_longlong),
    TEST_ENTRY(test_snprintf_truncation),
    TEST_ENTRY(test_snprintf_truncation_value),
    TEST_ENTRY(test_snprintf_null_zero),
    TEST_ENTRY(test_vsnprintf_basic),
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
