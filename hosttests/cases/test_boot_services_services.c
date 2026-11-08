/* hosttests/cases/test_boot_services_services.c — BOOT-5 Task 3 services test.
 *
 * Verifies kernel/init/services.c:
 *   - boot_services_prepare() calls net_lwip_init() once.
 *   - boot_services_start() calls arch_thermal_start_worker(), logs warning on error without fatal.
 */

#include "test_framework.h"
#include <init/services.h>
#include <errno.h>

static int net_lwip_init_calls = 0;
static int arch_thermal_start_worker_calls = 0;
static int mock_thermal_ret = 0;
static int log_warn_calls = 0;

void net_lwip_init(void)
{
    net_lwip_init_calls++;
}

int arch_thermal_start_worker(void)
{
    arch_thermal_start_worker_calls++;
    return mock_thermal_ret;
}

int g_log_level = 7;
void _log_warn_impl(const char *f, ...)
{
    (void)f;
    log_warn_calls++;
}
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

static void test_services_prepare(void)
{
    net_lwip_init_calls = 0;
    boot_services_prepare();
    assert_eq(1, net_lwip_init_calls);
}

static void test_services_start_success(void)
{
    arch_thermal_start_worker_calls = 0;
    log_warn_calls = 0;
    mock_thermal_ret = 0;

    boot_services_start();
    assert_eq(1, arch_thermal_start_worker_calls);
    assert_eq(0, log_warn_calls);
}

static void test_services_start_failure_warns(void)
{
    arch_thermal_start_worker_calls = 0;
    log_warn_calls = 0;
    mock_thermal_ret = -EIO;

    boot_services_start();
    assert_eq(1, arch_thermal_start_worker_calls);
    assert_eq(1, log_warn_calls);
}

int main(void)
{
    TEST_SUITE("BOOT-5 test_boot_services_services");

    test_services_prepare();
    test_services_start_success();
    test_services_start_failure_warns();

    TEST_RESULTS();
    return 0;
}
