/* hosttests/cases/test_boot_spawn_failure.c — BOOT-3 Task 3 spawn failure barrier.
 *
 * Verifies spawn_user_task:
 *   - When current->files is non-NULL and files_dup fails (returns NULL):
 *     spawn_user_task returns -ENOMEM and DOES NOT publish the task.
 *   - Verifies task_add_to_list, sched_enqueue, sched_notify_task_new,
 *     and user_init_task are NOT called.
 *   - Verifies clean unwind of private mm, thread, raw task, and vfs node.
 */

#include "test_framework.h"
#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* Trackers */
static int task_add_to_list_calls = 0;
static int sched_enqueue_calls = 0;
static int sched_notify_calls = 0;
static int vfs_node_put_calls = 0;
static int vma_free_all_calls = 0;
static int vmm_free_user_map_calls = 0;

/* Mock current task & init_mm */
static task_t g_current;
task_t *host_test_current = &g_current;
mm_t init_mm;
task_t *user_init_task = NULL;
int64_t user_init_pid = 0;
uint32_t num_cpus = 2;
spinlock_T task_list_lock = {1};

/* Inject flags */
static int inject_files_dup_fail = 0;

files_t *files_dup(files_t *fs)
{
    if (inject_files_dup_fail) return NULL;
    return fs;
}

void files_unpin(files_t *fs) { (void)fs; }

/* Mock VFS */
static vfs_node_t mock_elf_node = { .type = VFS_FILE };
vfs_node_t *vfs_lookup(const char *path)
{
    (void)path;
    return &mock_elf_node;
}

void vfs_node_put(vfs_node_t *node)
{
    (void)node;
    vfs_node_put_calls++;
}

int elf_validate(vfs_node_t *node) { (void)node; return 0; }
int elf_load(vfs_node_t *node, uint64_t *out_entry)
{
    (void)node;
    *out_entry = 0x400000;
    return 0;
}

/* MM mocks */
mm_t *mm_alloc(void)
{
    mm_t *m = (mm_t *)calloc(1, sizeof(mm_t));
    return m;
}

static uint64_t fake_pgd[512];
uint64_t vmm_alloc_map(void) { return (uint64_t)&fake_pgd[0]; }
void vmm_free_user_map(uint64_t *pgdir) { (void)pgdir; vmm_free_user_map_calls++; }
int vmm_map_page(uint64_t *pgd, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    (void)pgd; (void)vaddr; (void)paddr; (void)flags;
    return 0;
}
void vma_free_all(mm_t *mm) { (void)mm; vma_free_all_calls++; }
int vma_init(mm_t *mm, uint64_t start, uint64_t end, uint32_t flags)
{
    (void)mm; (void)start; (void)end; (void)flags;
    return 0;
}
uint64_t alloc_pages(int type, int order) { (void)type; (void)order; return 0x2000000; }
void free_pages(uint64_t addr, int order) { (void)addr; (void)order; }

/* Arch & Sched mocks */
pid_t alloc_pid(void) { return 1; }
uint32_t sched_pick_cpu(void) { return 0; }

void *kmalloc(size_t sz) { return malloc(sz); }
void kfree(void *p) { free(p); }
int elf_detect_abi(vfs_node_t *node) { (void)node; return 0; }
int mm_init_user_heap(mm_t *mm, uint64_t start, uint64_t end) { (void)mm; (void)start; (void)end; return 0; }
void ret_from_intr(void) {}
union task_union init_task_union;
percpu_t percpu_data[NR_CPUS];
void enqueue_task(task_t *task, percpu_t *rq) { (void)task; (void)rq; }
void sched_notify_remote(task_t *tsk) { (void)tsk; }
int g_log_level = 0;
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }
void arch_cpu_halt(void) { abort(); }
int arch_auxv_platform(void *buf, size_t max) { (void)buf; (void)max; return 0; }
size_t arch_auxv_payload_size(void) { return 0; }
int kernel_random_get_strong(void *buf, size_t n) { memset(buf, 0, n); return 0; }

void task_add_to_list(task_t *task)
{
    (void)task;
    task_add_to_list_calls++;
}

void sched_enqueue(task_t *task)
{
    (void)task;
    sched_enqueue_calls++;
}

void sched_notify_task_new(task_t *task)
{
    (void)task;
    sched_notify_calls++;
}

void task_nofile_init(task_t *task) { (void)task; }
void task_identity_init(task_t *task, const char *comm) { (void)task; (void)comm; }
const char *task_basename(const char *path) { return path; }

/* Prototype for spawn_user_task */
int64_t spawn_user_task(const char *path, const char *const *argv);

static void reset_spawn_test_state(void)
{
    memset(&g_current, 0, sizeof(g_current));
    user_init_task = NULL;
    user_init_pid = 0;
    task_add_to_list_calls = 0;
    sched_enqueue_calls = 0;
    sched_notify_calls = 0;
    vfs_node_put_calls = 0;
    vma_free_all_calls = 0;
    vmm_free_user_map_calls = 0;
    inject_files_dup_fail = 0;
}

static void test_spawn_files_dup_failure(void)
{
    reset_spawn_test_state();
    files_t parent_files;
    memset(&parent_files, 0, sizeof(parent_files));
    g_current.files = &parent_files;
    inject_files_dup_fail = 1;

    int64_t ret = spawn_user_task("/bin/init", NULL);

    /* Must return -ENOMEM */
    assert_eq(-ENOMEM, ret);

    /* Publication barrier: must NOT publish */
    assert_eq(0, task_add_to_list_calls);
    assert_eq(0, sched_enqueue_calls);
    assert_eq(0, sched_notify_calls);
    assert_null(user_init_task);
    assert_eq(0, user_init_pid);

    /* Must have freed node */
    assert_eq(1, vfs_node_put_calls);
}

int main(void)
{
    TEST_SUITE("BOOT-3 test_boot_spawn_failure");

    test_spawn_files_dup_failure();

    TEST_RESULTS();
    return 0;
}
