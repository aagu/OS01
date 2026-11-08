/* hosttests/cases/test_boot_stdio.c — BOOT-3 Task 3 stdio transaction host test.
 *
 * Verifies sched_boot_prepare_stdio:
 *   - Rejects NULL idle target or idle with existing files ("target", -EINVAL)
 *   - Recovers cleanly when files_alloc fails ("files", -ENOMEM)
 *   - Recovers cleanly when vfs_lookup fails ("tty-node", -ENOENT)
 *   - Recovers cleanly when file_alloc fails on 0, 1, 2 ("file-0/1/2", -ENOMEM)
 *   - Recovers cleanly when fd_alloc_limit fails on 0, 1, 2 ("fd-0/1/2", -EMFILE)
 *   - On success, idle->files is attached with fd 0 (O_RDWR), 1 (O_WRONLY), 2 (O_WRONLY)
 *     and reference counts are balanced.
 */

#include "test_framework.h"
#include <sched/boot_stdio.h>
#include <sched/task.h>
#include <sched/resource.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <errno.h>
#include <string.h>

/* Mock VFS node for /dev/tty */
static vfs_node_t mock_tty_node;
static int mock_tty_node_refs = 0;
static int vfs_lookup_fail = 0;

vfs_node_t *vfs_lookup(const char *path)
{
    if (strcmp(path, "/dev/tty") == 0) {
        if (vfs_lookup_fail) return NULL;
        mock_tty_node_refs++;
        return &mock_tty_node;
    }
    return NULL;
}

vfs_node_t *vfs_node_get(vfs_node_t *node)
{
    if (node == &mock_tty_node) {
        mock_tty_node_refs++;
    }
    return node;
}

void vfs_node_put(vfs_node_t *node)
{
    if (node == &mock_tty_node) {
        mock_tty_node_refs--;
    }
}

void devfs_release_file(file_t *f) { (void)f; }
void kfree(void *p) { free(p); }
void netbuf_delete(void *p) { (void)p; }
void netconn_delete(void *p) { (void)p; }
void wait_queue_wake_one(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_all(wait_queue_t *wq) { (void)wq; }
void task_nofile_init(task_t *tsk) { if (tsk) tsk->nofile_soft = 64; }
uint64_t task_nofile_soft(task_t *tsk) { if (!tsk) return 0; return tsk->nofile_soft; }
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }

/* Injectable allocators */
static int inject_files_alloc_fail = 0;
static int inject_file_alloc_fail_idx = -1; /* fail on Nth file_alloc (0, 1, 2) */
static int file_alloc_calls = 0;

/* Wrapper / override for files_alloc if needed, or intercepted via macro / malloc */
extern files_t *__real_files_alloc(void);
extern file_t *__real_file_alloc(void);

/* We will hook malloc / calloc or wrap if needed. But in file.c:
 * files_alloc calls calloc(1, sizeof(files_t))
 * file_alloc calls calloc(1, sizeof(file_t))
 */
static int calloc_hook_active = 0;
static int fail_calloc_file_count = -1;
static int seen_calloc_file_count = 0;

void *__wrap_calloc(size_t nmemb, size_t size)
{
    extern void *__real_calloc(size_t nmemb, size_t size);
    if (calloc_hook_active) {
        if (size == sizeof(files_t) && inject_files_alloc_fail) {
            return NULL;
        }
        if (size == sizeof(file_t)) {
            if (seen_calloc_file_count == fail_calloc_file_count) {
                seen_calloc_file_count++;
                return NULL;
            }
            seen_calloc_file_count++;
        }
    }
    return __real_calloc(nmemb, size);
}

/* Logging mocks */
int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

static task_t test_idle_task;

static void reset_stdio_test_state(void)
{
    memset(&test_idle_task, 0, sizeof(test_idle_task));
    task_nofile_init(&test_idle_task);
    mock_tty_node_refs = 0;
    vfs_lookup_fail = 0;
    inject_files_alloc_fail = 0;
    fail_calloc_file_count = -1;
    seen_calloc_file_count = 0;
    calloc_hook_active = 1;
}

static void test_preconditions(void)
{
    reset_stdio_test_state();
    const char *failed = NULL;
    int rc = sched_boot_prepare_stdio(NULL, &failed);
    assert_eq(-EINVAL, rc);
    assert_str_eq("target", failed ? failed : "");

    reset_stdio_test_state();
    files_t existing_files;
    memset(&existing_files, 0, sizeof(existing_files));
    test_idle_task.files = &existing_files;
    failed = NULL;
    rc = sched_boot_prepare_stdio(&test_idle_task, &failed);
    assert_eq(-EINVAL, rc);
    assert_str_eq("target", failed ? failed : "");
    assert_true(test_idle_task.files == &existing_files);
}

static void test_files_alloc_fail(void)
{
    reset_stdio_test_state();
    inject_files_alloc_fail = 1;
    const char *failed = NULL;
    int rc = sched_boot_prepare_stdio(&test_idle_task, &failed);
    assert_eq(-ENOMEM, rc);
    assert_str_eq("files", failed ? failed : "");
    assert_null(test_idle_task.files);
    assert_eq(0, mock_tty_node_refs);
}

static void test_vfs_lookup_fail(void)
{
    reset_stdio_test_state();
    vfs_lookup_fail = 1;
    const char *failed = NULL;
    int rc = sched_boot_prepare_stdio(&test_idle_task, &failed);
    assert_eq(-ENOENT, rc);
    assert_str_eq("tty-node", failed ? failed : "");
    assert_null(test_idle_task.files);
    assert_eq(0, mock_tty_node_refs);
}

static void test_file_alloc_failures(void)
{
    const char *components[3] = { "file-0", "file-1", "file-2" };
    for (int i = 0; i < 3; i++) {
        reset_stdio_test_state();
        fail_calloc_file_count = i;
        const char *failed = NULL;
        int rc = sched_boot_prepare_stdio(&test_idle_task, &failed);
        assert_eq(-ENOMEM, rc);
        assert_str_eq(components[i], failed ? failed : "");
        assert_null(test_idle_task.files);
        assert_eq(0, mock_tty_node_refs);
    }
}

static void test_fd_limit_failures(void)
{
    /* Limit soft limit to 0, 1, 2 */
    const char *components[3] = { "fd-0", "fd-1", "fd-2" };
    for (int limit = 0; limit < 3; limit++) {
        reset_stdio_test_state();
        test_idle_task.nofile_soft = limit;
        const char *failed = NULL;
        int rc = sched_boot_prepare_stdio(&test_idle_task, &failed);
        assert_eq(-EMFILE, rc);
        assert_str_eq(components[limit], failed ? failed : "");
        assert_null(test_idle_task.files);
        assert_eq(0, mock_tty_node_refs);
    }
}

static void test_success_path(void)
{
    reset_stdio_test_state();
    const char *failed = "garbage";
    int rc = sched_boot_prepare_stdio(&test_idle_task, &failed);
    assert_eq(0, rc);
    assert_not_null(test_idle_task.files);

    files_t *files = test_idle_task.files;
    assert_eq(1, files->refcount);

    assert_not_null(files->fd[0]);
    assert_not_null(files->fd[1]);
    assert_not_null(files->fd[2]);
    assert_null(files->fd[3]);

    assert_eq(O_RDWR, files->fd[0]->flags);
    assert_eq(O_WRONLY, files->fd[1]->flags);
    assert_eq(O_WRONLY, files->fd[2]->flags);

    assert_true(files->fd[0]->node == &mock_tty_node);
    assert_true(files->fd[1]->node == &mock_tty_node);
    assert_true(files->fd[2]->node == &mock_tty_node);

    /* Total refs held on tty should be exactly 3 (one per fd slot, lookup ref released) */
    assert_eq(3, mock_tty_node_refs);

    /* Cleanup */
    files_unpin(files);
    test_idle_task.files = NULL;
    assert_eq(0, mock_tty_node_refs);
}

int main(void)
{
    TEST_SUITE("BOOT-3 test_boot_stdio");

    test_preconditions();
    test_files_alloc_fail();
    test_vfs_lookup_fail();
    test_file_alloc_failures();
    test_fd_limit_failures();
    test_success_path();

    TEST_RESULTS();
    return 0;
}
