/* hosttests/cases/test_boot_failure_terminal.c — BOOT-3 Task 2.
 *
 * Verifies terminal and PTY failure propagation:
 *   - tty_alloc failure does not register nodes or bind globals.
 *   - devfs -EEXIST on /dev/tty or /dev/tty0 is treated as failure.
 *   - partial registration failure retains object and does not bind globals.
 *   - pty_init propagates devfs registration errors.
 */

#include "test_framework.h"
#include <tty/boot.h>
#include <tty/pty.h>
#include <tty/tty.h>
#include <fs/devfs.h>
#include <fs/file.h>
#include <sched/task.h>
#include <errno.h>
#include <string.h>

/* Mock tty object */
static tty_t mock_console_obj;
static int mock_tty_alloc_fail = 0;
static int tty_alloc_calls = 0;

tty_t *tty_alloc(void (*output_char)(char), void (*echo_char)(char))
{
    (void)output_char; (void)echo_char;
    tty_alloc_calls++;
    if (mock_tty_alloc_fail) return NULL;
    return &mock_console_obj;
}

void console_putchar(char c) { (void)c; }

const struct devfs_ops tty_magic_ops = {0};
const struct devfs_ops tty_phys_ops = {0};

/* Stubs for pty.c external dependencies */
spinlock_T pty_lock = {1};
pipe_t *pipe_alloc(void) { return NULL; }
void pipe_free(pipe_t *p) { (void)p; }
file_t *file_alloc(void) { return NULL; }
void file_free(file_t *f) { (void)f; }
int signal_pgrp(pid_t pgrp, int sig) { (void)pgrp; (void)sig; return 0; }
int syscall_check_user_range(const void *uptr, size_t size, int write) { (void)uptr; (void)size; (void)write; return 0; }
long copy_to_user_ft_res(void *dst, const void *src, size_t n) { (void)dst; (void)src; (void)n; return 0; }
long copy_from_user_ft_res(void *dst, const void *src, size_t n) { (void)dst; (void)src; (void)n; return 0; }
task_t *host_test_current = NULL;
task_t *gfx_test_current = NULL;
tty_t *keyboard_get_tty(void) { return NULL; }

/* Mock devfs registration */
static int mock_register_tty_rc = 0;
static int mock_register_tty0_rc = 0;
static int mock_register_ptmx_rc = 0;
static int registered_tty_calls = 0;
static int registered_tty0_calls = 0;
static int registered_ptmx_calls = 0;
static void *registered_tty_private = NULL;
static void *registered_tty0_private = NULL;

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops)
{
    (void)ops;
    if (strcmp(name, "tty") == 0) {
        registered_tty_calls++;
        registered_tty_private = private_data;
        return mock_register_tty_rc;
    }
    if (strcmp(name, "tty0") == 0) {
        registered_tty0_calls++;
        registered_tty0_private = private_data;
        return mock_register_tty0_rc;
    }
    if (strcmp(name, "ptmx") == 0) {
        registered_ptmx_calls++;
        return mock_register_ptmx_rc;
    }
    return -EINVAL;
}

/* Mock bindings */
static tty_t *bound_serial_tty = NULL;
static tty_t *bound_keyboard_tty = NULL;
static tty_t *bound_dev_tty = NULL;

void serial_set_tty(tty_t *tty) { bound_serial_tty = tty; }
void keyboard_set_tty(tty_t *tty) { bound_keyboard_tty = tty; }
void tty_set_dev_tty(tty_t *tty) { bound_dev_tty = tty; }

/* Log mocks */
int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }

static void reset_term_test_state(void)
{
    memset(&mock_console_obj, 0, sizeof(mock_console_obj));
    mock_tty_alloc_fail = 0;
    tty_alloc_calls = 0;

    mock_register_tty_rc = 0;
    mock_register_tty0_rc = 0;
    mock_register_ptmx_rc = 0;
    registered_tty_calls = 0;
    registered_tty0_calls = 0;
    registered_ptmx_calls = 0;
    registered_tty_private = NULL;
    registered_tty0_private = NULL;

    bound_serial_tty = NULL;
    bound_keyboard_tty = NULL;
    bound_dev_tty = NULL;
}

static void test_tty_alloc_null(void)
{
    reset_term_test_state();
    mock_tty_alloc_fail = 1;

    int rc = tty_boot_init();

    assert_eq(-ENOMEM, rc);
    assert_eq(1, tty_alloc_calls);
    assert_eq(0, registered_tty_calls);
    assert_eq(0, registered_tty0_calls);
    assert_null(bound_serial_tty);
    assert_null(bound_keyboard_tty);
    assert_null(bound_dev_tty);
}

static void test_tty_register_tty_eexist(void)
{
    reset_term_test_state();
    mock_register_tty_rc = -EEXIST;

    int rc = tty_boot_init();

    assert_eq(-EEXIST, rc);
    assert_eq(1, registered_tty_calls);
    assert_eq(0, registered_tty0_calls);
    assert_null(bound_serial_tty);
    assert_null(bound_keyboard_tty);
    assert_null(bound_dev_tty);
}

static void test_tty_register_tty0_eexist(void)
{
    reset_term_test_state();
    mock_register_tty0_rc = -EEXIST;

    int rc = tty_boot_init();

    assert_eq(-EEXIST, rc);
    assert_eq(1, registered_tty_calls);
    assert_eq(1, registered_tty0_calls);
    /* Partial registration: tty is registered, but globals must NOT be bound */
    assert_not_null(registered_tty_private);
    assert_null(bound_serial_tty);
    assert_null(bound_keyboard_tty);
    assert_null(bound_dev_tty);
}

static void test_tty_success(void)
{
    reset_term_test_state();

    int rc = tty_boot_init();

    assert_eq(0, rc);
    assert_eq(1, registered_tty_calls);
    assert_eq(1, registered_tty0_calls);
    assert_eq(&mock_console_obj, registered_tty_private);
    assert_eq(&mock_console_obj, registered_tty0_private);
    assert_eq(&mock_console_obj, bound_serial_tty);
    assert_eq(&mock_console_obj, bound_keyboard_tty);
    assert_eq(&mock_console_obj, bound_dev_tty);
}

static void test_pty_init_eexist(void)
{
    reset_term_test_state();
    mock_register_ptmx_rc = -EEXIST;

    int rc = pty_init();

    assert_eq(-EEXIST, rc);
    assert_eq(1, registered_ptmx_calls);
}

static void test_pty_init_enomem(void)
{
    reset_term_test_state();
    mock_register_ptmx_rc = -ENOMEM;

    int rc = pty_init();

    assert_eq(-ENOMEM, rc);
    assert_eq(1, registered_ptmx_calls);
}

static void test_pty_init_success(void)
{
    reset_term_test_state();

    int rc = pty_init();

    assert_eq(0, rc);
    assert_eq(1, registered_ptmx_calls);
}

int main(void)
{
    TEST_SUITE("BOOT-3 boot_failure_terminal");
    test_tty_alloc_null();
    test_tty_register_tty_eexist();
    test_tty_register_tty0_eexist();
    test_tty_success();
    test_pty_init_eexist();
    test_pty_init_enomem();
    test_pty_init_success();
    TEST_RESULTS();
    return 0;
}
