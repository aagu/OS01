/*
 * test/cases/test_libc_vsprintf.c — vsprintf format string tests
 */
#include "test_framework.h"
#include <string.h>
#include <stdarg.h>

extern int vsprintf(char *buf, const char *fmt, va_list args);

static int my_sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    int ret;
    va_start(args, fmt);
    ret = vsprintf(buf, fmt, args);
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

TEST_LIST_BEGIN
    TEST_ENTRY(test_sprintf_strings),
    TEST_ENTRY(test_sprintf_integers),
    TEST_ENTRY(test_sprintf_hex),
    TEST_ENTRY(test_sprintf_multiple),
    TEST_ENTRY(test_sprintf_char),
    TEST_ENTRY(test_sprintf_ptr),
    TEST_ENTRY(test_sprintf_percent),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
