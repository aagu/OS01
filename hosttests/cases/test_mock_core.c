/* @os01-test
 * id: test_mock_core
 * layer: host
 * subsystem: framework
 * timing: strict
 * tier: pr
 * mock: core
 * scenarios: [framework.mock.core]
 */
#include <test_framework.h>
#include <mock_core.h>

extern void *kmalloc(size_t size);
extern size_t kfree(void *addr);
extern int color_printk(unsigned int fr, unsigned int bk, const char *fmt, ...);
extern int serial_printk(const char *fmt, ...);
extern int g_log_level;
extern void _log_write(int level, const char *fmt, ...);
extern void _log_err_impl(const char *fmt, ...);
extern void _log_warn_impl(const char *fmt, ...);
extern void _log_info_impl(const char *fmt, ...);
extern void log_set_level(int level);
extern int log_get_level(void);

TEST_FUNC(test_kmalloc_counting_and_fail_next)
{
    mock_reset_all();
    assert_eq(0, mock_calls("kmalloc"));
    assert_eq(0, mock_calls("kfree"));

    void *p1 = kmalloc(32);
    assert_not_null(p1);
    assert_eq(1, mock_calls("kmalloc"));
    kfree(p1);
    assert_eq(1, mock_calls("kfree"));

    mock_fail_next("kmalloc", 1);
    void *p2 = kmalloc(64);
    assert_null(p2);
    assert_eq(2, mock_calls("kmalloc"));

    void *p3 = kmalloc(64);
    assert_not_null(p3);
    assert_eq(3, mock_calls("kmalloc"));
    kfree(p3);
}

TEST_FUNC(test_printk_capture)
{
    mock_reset_all();
    assert_str_eq("", mock_printk_last());

    serial_printk("hello %s %d", "serial", 42);
    assert_str_eq("hello serial 42", mock_printk_last());

    color_printk(0x00ffffff, 0x00000000, "color=%d", 7);
    assert_str_eq("color=7", mock_printk_last());
}

TEST_FUNC(test_reset_all_clears_state)
{
    mock_reset_all();
    void *p = kmalloc(16);
    assert_not_null(p);
    kfree(p);
    serial_printk("before reset");
    mock_fail_next("kmalloc", 2);

    mock_reset_all();
    assert_eq(0, mock_calls("kmalloc"));
    assert_eq(0, mock_calls("kfree"));
    assert_str_eq("", mock_printk_last());

    void *p2 = kmalloc(16);
    assert_not_null(p2);
    kfree(p2);
}

TEST_FUNC(test_log_noop_cluster)
{
    mock_reset_all();
    log_set_level(3);
    assert_eq(3, log_get_level());
    assert_eq(3, g_log_level);
    _log_write(3, "noop %d", 1);
    _log_err_impl("err %s", "msg");
    _log_warn_impl("warn");
    _log_info_impl("info");
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_kmalloc_counting_and_fail_next),
    TEST_ENTRY(test_printk_capture),
    TEST_ENTRY(test_reset_all_clears_state),
    TEST_ENTRY(test_log_noop_cluster),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("mock-core");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
