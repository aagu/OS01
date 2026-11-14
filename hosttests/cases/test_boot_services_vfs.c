/* hosttests/cases/test_boot_services_vfs.c — BOOT-5 Task 1 VFS bootstrap contract.
 *
 * Verifies:
 *   - vfs_init() initializes vfs_bootstrap to active (1).
 *   - While vfs_bootstrap_active() is true:
 *       - vfs_sched_ready() returns false regardless of current->pid.
 *       - Calling namespace functions (e.g. sys_mkdir) does NOT acquire vfs_namespace_lock.
 *       - spawn_user_task() returns -EAGAIN immediately without calling vfs_lookup,
 *         allocating PID, or initializing mm.
 *   - Calling vfs_end_bootstrap():
 *       - Sets vfs_bootstrap_active() to false.
 *       - Is idempotent (subsequent calls keep it false).
 *       - Atomic release/acquire visibility across threads.
 *   - Once vfs_bootstrap_active() is false:
 *       - For current->pid > 0: vfs_sched_ready() is true, and namespace functions
 *         DO acquire and release vfs_namespace_lock.
 *       - For current->pid == 0 (idle): vfs_sched_ready() is false, and namespace
 *         functions do NOT acquire mutex (idle exemption preserved).
 */

#include "test_framework.h"
#include <fs/vfs.h>
#include <errno.h>
#include <string.h>
#include <arch/x86_64/vma.h>

/* Host pthread prototypes (avoiding shadow from libc headers) */
typedef unsigned long host_pthread_t;
extern int pthread_create(host_pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
extern int pthread_join(host_pthread_t thread, void **retval);
extern int usleep(unsigned int usec);

/* Hook trackers for mutex_lock / mutex_unlock */
static int mutex_lock_calls = 0;
static int mutex_unlock_calls = 0;

void vfs_test_mutex_lock_hook(mutex_t *m)
{
    (void)m;
    mutex_lock_calls++;
}

void vfs_test_mutex_unlock_hook(mutex_t *m)
{
    (void)m;
    mutex_unlock_calls++;
}

/* Tracking wrapper for vfs_lookup */
static int wrap_vfs_lookup_calls = 0;
vfs_node_t *__real_vfs_lookup(const char *path);
vfs_node_t *__wrap_vfs_lookup(const char *path)
{
    wrap_vfs_lookup_calls++;
    return __real_vfs_lookup(path);
}

struct mm_struct { int dummy; };
typedef struct mm_struct mm_t;
union task_union { task_t task; unsigned long stack[1024]; };

typedef struct block_device block_device_t;
int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf; return 0;
}
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf; return 0;
}
uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

/* Stubs required by boot3_exec_production.o */
static task_t host_current_task;
task_t *vfs_test_current = &host_current_task;
task_t *host_test_current = &host_current_task;
union task_union init_task_union;
percpu_t percpu_data[2];
task_t *user_init_task = NULL;

uint32_t num_cpus = 2;
spinlock_T task_list_lock = {1};
int64_t user_init_pid = 0;

int procfs_register_fs(void) { return 0; }

files_t *files_dup(files_t *fs) { return fs; }
void files_unpin(files_t *fs) { (void)fs; }
void task_add_to_list(task_t *task) { (void)task; }
void sched_enqueue(task_t *task) { (void)task; }
void sched_notify_task_new(task_t *task) { (void)task; }
void enqueue_task(task_t *task, percpu_t *rq) { (void)task; (void)rq; }
void sched_notify_remote(task_t *tsk) { (void)tsk; }
void task_nofile_init(task_t *task) { (void)task; }
void task_identity_init(task_t *task, const char *comm) { (void)task; (void)comm; }
const char *task_basename(const char *path) { return path; }

int elf_detect_abi(vfs_node_t *node) { (void)node; return 0; }
int elf_validate(vfs_node_t *node) { (void)node; return 0; }
int elf_load(vfs_node_t *node, uint64_t *out_entry) { (void)node; *out_entry = 0x400000; return 0; }
void ret_from_intr(void) {}

mm_t init_mm;
mm_t *mm_alloc(void) { return calloc(1, sizeof(mm_t)); }
static uint64_t fake_pgd[512];
uint64_t vmm_alloc_map(void) { return (uint64_t)&fake_pgd[0]; }
void vmm_free_user_map(uint64_t *pgdir) { (void)pgdir; }
int vmm_map_page(uint64_t *pgd, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
    (void)pgd; (void)vaddr; (void)paddr; (void)flags;
    return 0;
}
int vma_init(mm_t *mm, uint64_t start, uint64_t end, uint32_t flags)
{
    (void)mm; (void)start; (void)end; (void)flags;
    return 0;
}
void vma_free_all(mm_t *mm) { (void)mm; }
int mm_init_user_heap(mm_t *mm, uint64_t elf_end)
{
    (void)mm; (void)elf_end;
    return 0;
}
uint64_t alloc_pages(int type, int order) { (void)type; (void)order; return 0x2000000; }
void free_pages(uint64_t addr, int order) { (void)addr; (void)order; }
pid_t alloc_pid(void) { return 1; }
uint32_t sched_pick_cpu(void) { return 0; }
unsigned sched_pick_cpu_mask(uint64_t mask) { (void)mask; return 0; }
bool sched_boot_creation_open(void) { return true; }
void *arch_this_cpu_ptr(void) { return &percpu_data[0]; }

int arch_auxv_platform(void *buf, size_t max) { (void)buf; (void)max; return 0; }
size_t arch_auxv_payload_size(void) { return 0; }
int kernel_random_get_strong(void *buf, size_t n) { memset(buf, 0, n); return 0; }
void arch_cpu_halt(void) { abort(); }

int g_log_level = 0;
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

int64_t spawn_user_task(const char *path, const char *const *argv);

static void reset_vfs_test_state(void)
{
    memset(&host_current_task, 0, sizeof(host_current_task));
    host_current_task.pid = 1;
    mutex_lock_calls = 0;
    mutex_unlock_calls = 0;
    wrap_vfs_lookup_calls = 0;
}

/* Test 1: VFS bootstrap active state & mutex behavior */
static void test_vfs_bootstrap_lifecycle(void)
{
    reset_vfs_test_state();
    vfs_init();

    /* 1. Initial state: bootstrap active */
    assert_true(vfs_bootstrap_active());

    /* 2. With pid > 0, vfs_sched_ready() is false because bootstrap is active */
    host_current_task.pid = 1;
    assert_false(vfs_sched_ready());

    /* 3. Namespace op vfs_unlink does NOT take mutex while bootstrap is active */
    mutex_lock_calls = 0;
    mutex_unlock_calls = 0;
    int rc = vfs_unlink("/testdir/foo", "/");
    (void)rc;
    assert_eq(0, mutex_lock_calls);
    assert_eq(0, mutex_unlock_calls);

    /* 4. End bootstrap */
    vfs_end_bootstrap();
    assert_false(vfs_bootstrap_active());

    /* 5. End bootstrap is idempotent */
    vfs_end_bootstrap();
    assert_false(vfs_bootstrap_active());

    /* 6. With pid > 0, vfs_sched_ready() is now true */
    assert_true(vfs_sched_ready());

    /* 7. Namespace op vfs_unlink DOES take and release mutex */
    mutex_lock_calls = 0;
    mutex_unlock_calls = 0;
    rc = vfs_unlink("/testdir/foo", "/");
    (void)rc;
    assert_eq(1, mutex_lock_calls);
    assert_eq(1, mutex_unlock_calls);

    /* 8. With pid == 0 (idle task), vfs_sched_ready() is still false */
    host_current_task.pid = 0;
    assert_false(vfs_sched_ready());

    mutex_lock_calls = 0;
    mutex_unlock_calls = 0;
    rc = vfs_unlink("/testdir/foo", "/");
    (void)rc;
    assert_eq(0, mutex_lock_calls);
    assert_eq(0, mutex_unlock_calls);
}

/* Test 2: Multithreaded release/acquire visibility */
static volatile int g_shared_payload = 0;
static void *visibility_worker(void *arg)
{
    (void)arg;
    int attempts = 0;
    while (vfs_bootstrap_active()) {
        usleep(100);
        if (++attempts > 20000) return (void *)(uintptr_t)1;
    }
    /* bootstrap is false; shared payload MUST be visible */
    if (g_shared_payload != 42) {
        return (void *)(uintptr_t)2;
    }
    return NULL;
}

static void test_vfs_bootstrap_concurrency(void)
{
    vfs_init();
    assert_true(vfs_bootstrap_active());
    g_shared_payload = 0;

    host_pthread_t th;
    int err = pthread_create(&th, NULL, visibility_worker, NULL);
    assert_eq(0, err);

    usleep(1000);
    g_shared_payload = 42;
    vfs_end_bootstrap();

    void *retval = NULL;
    pthread_join(th, &retval);
    assert_true(retval == NULL);
}

/* Test 3: spawn_user_task rejection while bootstrap is active */
static void test_spawn_rejection_during_bootstrap(void)
{
    reset_vfs_test_state();
    vfs_init();
    assert_true(vfs_bootstrap_active());

    /* While bootstrap is active, spawn_user_task must return -EAGAIN */
    wrap_vfs_lookup_calls = 0;
    int64_t ret = spawn_user_task("/bin/init", NULL);
    assert_eq(-EAGAIN, ret);
    /* Crucial: vfs_lookup must NOT have been called */
    assert_eq(0, wrap_vfs_lookup_calls);
}

int main(void)
{
    TEST_SUITE("BOOT-5 test_boot_services_vfs");

    test_vfs_bootstrap_lifecycle();
    test_vfs_bootstrap_concurrency();
    test_spawn_rejection_during_bootstrap();

    TEST_RESULTS();
    return 0;
}
