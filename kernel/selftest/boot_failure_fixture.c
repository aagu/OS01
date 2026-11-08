/* kernel/selftest/boot_failure_fixture.c — BOOT-3 startup failure fixture.
 *
 * Linked exclusively into boot-failure-<case> kernel variants.
 */

#include <selftest/boot_failure_fixture.h>
#include <subsys/subsys.h>
#include <core/boot_failure.h>
#include <log/log.h>
#include <string.h>
#include <errno.h>

#ifndef BOOT_FAILURE_CASE
#define BOOT_FAILURE_CASE "none"
#endif

static bool site_fired = false;
static bool in_stdio_win = false;
static int stdio_alloc_count = 0;
static bool in_spawn_win = false;

void boot3_test_sentinel(const char *sentinel)
{
    if (sentinel && sentinel[0] != '\0') {
        log_info("[boot-3-test] %s\n", sentinel);
    }
}

bool boot3_test_fail_site(const char *site)
{
    if (!site || site_fired)
        return false;

    bool match = false;
    const char *curr = BOOT_FAILURE_CASE;

    if (strcmp(curr, "required-subsys") == 0 && strcmp(site, "boot3-required") == 0)
        match = true;
    else if (strcmp(curr, "optional-subsys") == 0 && strcmp(site, "boot3-optional") == 0)
        match = true;
    else if (strcmp(curr, "registration") == 0 && strcmp(site, "boot3-duplicate") == 0)
        match = true;
    else if (strcmp(curr, "root") == 0 && strcmp(site, "root") == 0)
        match = true;
    else if (strcmp(curr, "tty") == 0 && strcmp(site, "console") == 0)
        match = true;
    else if (strcmp(curr, "pty") == 0 && strcmp(site, "ptmx") == 0)
        match = true;
    else if (strcmp(curr, "stdio") == 0 && strcmp(site, "file-1") == 0)
        match = true;
    else if (strcmp(curr, "init") == 0 && strcmp(site, "/bin/init") == 0)
        match = true;
    else if (strcmp(curr, "child-fds") == 0 && strcmp(site, "spawn-fds") == 0)
        match = true;

    if (match) {
        site_fired = true;
        log_info("[boot-3-test] injected=%s site=%s\n", curr, site);
        return true;
    }
    return false;
}

const char *boot3_test_init_path(void)
{
    if (strcmp(BOOT_FAILURE_CASE, "init") == 0) {
        boot3_test_fail_site("/bin/init");
        return "/bin/nonexistent_init";
    }
    return "/bin/init";
}

void boot3_test_stdio_enter(void)
{
    in_stdio_win = true;
    stdio_alloc_count = 0;
}

void boot3_test_stdio_leave(void)
{
    in_stdio_win = false;
}

bool boot3_test_in_stdio_window(void)
{
    if (!in_stdio_win)
        return false;
    stdio_alloc_count++;
    if (stdio_alloc_count == 2) {
        if (strcmp(BOOT_FAILURE_CASE, "stdio") == 0) {
            boot3_test_fail_site("file-1");
            return true;
        }
    }
    return false;
}

void boot3_test_spawn_enter(void)
{
    in_spawn_win = true;
}

void boot3_test_spawn_leave(void)
{
    in_spawn_win = false;
}

bool boot3_test_in_spawn_window(void)
{
    return in_spawn_win;
}

/* Subsystem test callbacks */
static int boot3_required_cb(void)
{
    boot3_test_fail_site("boot3-required");
    return -EIO;
}

static int boot3_optional_cb(void)
{
    boot3_test_fail_site("boot3-optional");
    return -EIO;
}

static int boot3_optional_continuation_cb(void)
{
    int raw = subsys_result("boot3-optional");
    int st = subsys_status("boot3-optional");
    log_info("[boot-3-test] optional-subsys-continued status=%d raw=%d\n", st, raw);
    return 0;
}

static int boot3_duplicate_cb(void)
{
    return 0;
}

void boot3_test_early_init(void)
{
    log_info("[boot-3-test] variant=boot-failure-%s\n", BOOT_FAILURE_CASE);

    if (strcmp(BOOT_FAILURE_CASE, "required-subsys") == 0) {
        register_subsys("boot3-required", boot3_required_cb, SUBSYS_PHASE_5, 0);
    } else if (strcmp(BOOT_FAILURE_CASE, "optional-subsys") == 0) {
        register_subsys("boot3-optional", boot3_optional_cb, SUBSYS_PHASE_5, SUBSYS_FLAG_OPTIONAL);
        register_subsys("boot3-opt-cont", boot3_optional_continuation_cb, SUBSYS_PHASE_5, 0);
    } else if (strcmp(BOOT_FAILURE_CASE, "registration") == 0) {
        register_subsys("boot3-duplicate", boot3_duplicate_cb, SUBSYS_PHASE_5, 0);
        boot3_test_fail_site("boot3-duplicate");
        /* Duplicate registration to trigger latch */
        register_subsys("boot3-duplicate", boot3_duplicate_cb, SUBSYS_PHASE_5, 0);
    }
}
