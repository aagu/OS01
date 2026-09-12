/*
 * test/cases/test_libc_string.c — libc string function tests
 */
#include "test_framework.h"

extern void* memset(void* bufptr, int value, size_t size);
extern void* memcpy(void* dstptr, const void* srcptr, size_t size);
extern void* memmove(void* dstptr, const void* srcptr, size_t size);
extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, size_t n);
extern char* strcpy(char* dest, const char* src);

TEST_FUNC(test_memset) {
    char buf[16];
    memset(buf, 0xAB, 6);
    assert_eq(buf[0], (char)0xAB);
    assert_eq(buf[5], (char)0xAB);
    buf[6] = 0xCC;
    assert_eq(buf[6], (char)0xCC);
    memset(buf, 0, 16);
    for (int i = 0; i < 16; i++)
        assert_eq(buf[i], (char)0);
}

TEST_FUNC(test_memcpy) {
    char src[] = "Hello World";
    char dst[16] = {0};
    memcpy(dst, src, 12);
    assert_mem_eq(dst, src, 12);
}

TEST_FUNC(test_memmove_forward) {
    char buf[16];
    memset(buf, 0, 16);
    memcpy(buf, "ABCDEFGHIJ", 10);
    memmove(buf + 3, buf, 7);
    assert_eq(buf[3], 'A');
    assert_eq(buf[9], 'G');
}

TEST_FUNC(test_memmove_backward) {
    char buf[16];
    memset(buf, 0, 16);
    memcpy(buf, "ABCDEFGHIJ", 10);
    memmove(buf, buf + 3, 7);
    assert_eq(buf[0], 'D');
    assert_eq(buf[6], 'J');
}

TEST_FUNC(test_strlen) {
    assert_eq(strlen(""), 0);
    assert_eq(strlen("a"), 1);
    assert_eq(strlen("Hello"), 5);
}

TEST_FUNC(test_strcmp) {
    assert_eq(strcmp("", ""), 0);
    assert_eq(strcmp("abc", "abc"), 0);
    assert_true(strcmp("abc", "abd") < 0);
    assert_true(strcmp("abd", "abc") > 0);
}

TEST_FUNC(test_strncmp) {
    assert_eq(strncmp("abc", "abc", 3), 0);
    assert_eq(strncmp("abc", "abd", 2), 0);
    assert_true(strncmp("abc", "abd", 3) < 0);
}

TEST_FUNC(test_strcpy) {
    char buf[16];
    memset(buf, 0xAA, 16);
    strcpy(buf, "test");
    // KNOWN BUG: strcpy doesn't null-terminate!
    // The do-while checks *src != 0 AFTER incrementing dest,
    // so '\0' is never written to dest.
    assert_eq(buf[0], 't');
    assert_eq(buf[1], 'e');
    assert_eq(buf[2], 's');
    assert_eq(buf[3], 't');
    assert_eq(buf[4], (char)0);  // null terminator
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_memset),
    TEST_ENTRY(test_memcpy),
    TEST_ENTRY(test_memmove_forward),
    TEST_ENTRY(test_memmove_backward),
    TEST_ENTRY(test_strlen),
    TEST_ENTRY(test_strcmp),
    TEST_ENTRY(test_strncmp),
    TEST_ENTRY(test_strcpy),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
