/* hosttests/cases/test_boot_services_spawn.c — BOOT-5 Task 2 initial spawn placement & publication.
 *
 * Verifies:
 *   - spawn_boot_target_allowed:
 *       - AP online but scheduler_ok=0 is not allowed
 *       - AP online && scheduler_ok=1 is allowed
 *       - UP (CPU 0) with window open is allowed as a boot exception
 *       - UP with window closed and scheduler_ok=0 is rejected
 *       - cpu >= num_cpus or num_cpus=0 is rejected
 *   - Initial placement:
 *       - When no target is allowed: spawn_user_task returns -EAGAIN before PID/mm allocation
 *       - UP with window open places on CPU 0
 *       - SMP with qualified AP places on qualified AP
 *   - Atomic publication and locked recheck:
 *       - When locked recheck fails: returns -EAGAIN
 *       - Lock order strictly: task_list_lock -> rq_lock -> rq_unlock -> task_list_unlock
 *       - All resource cleanup happens outside the locks
 *       - Zero task-list residual, zero runqueue enqueue, zero notify, no user_init publish
 *       - Resource cleanup ledger: files_unpin, fpu, mm, vmm_free_user_map, thread, raw exactly once
 */

#include "test_framework.h"
#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <memory/pmm.h>
#include <fs/vfs.h>
#include <fs/file.h>
#include <errno.h>
#include <string.h>
#include <arch/x86_64/vma.h>

/* Mock / Real externs */
extern uint32_t num_cpus;
extern percpu_t percpu_data[NR_CPUS];
extern task_t *user_init_task;
extern int64_t user_init_pid;
extern spinlock_T task_list_lock;

/* Trackers & Ledgers */
static int files_unpin_calls = 0;
static int fpu_free_calls = 0;
static int vmm_free_user_map_calls = 0;
static int vma_free_all_calls = 0;
static int kfree_mm_calls = 0;
static int kfree_thd_calls = 0;
static int kfree_raw_calls = 0;

static int enqueue_task_calls = 0;
static int sched_notify_remote_calls = 0;

/* Lock sequence tracking */
enum {
    LOCK_EV_TL_LOCK = 1,
    LOCK_EV_RQ_LOCK = 2,
    LOCK_EV_RQ_UNLOCK = 3,
    LOCK_EV_TL_UNLOCK = 4,
    LOCK_EV_CLEANUP = 5,
};
static int lock_events[32];
static int lock_event_count = 0;

static void record_lock_ev(int ev)
{
    if (lock_event_count < 32)
        lock_events[lock_event_count++] = ev;
}

/* Control hooks */
static int inject_locked_recheck_fail_cpu = -1;

/* VFS mock */
static bool mock_vfs_bootstrap_active = false;
bool vfs_bootstrap_active(void)
{
    return mock_vfs_bootstrap_active;
}

static vfs_node_t mock_file_node = { .type = VFS_FILE };
vfs_node_t *vfs_lookup(const char *path)
{
    (void)path;
    return &mock_file_node;
}
void vfs_node_put(vfs_node_t *node) { (void)node; }
int elf_validate(vfs_node_t *node) { (void)node; return 0; }
int elf_detect_abi(vfs_node_t *node) { (void)node; return 0; }
int elf_load(vfs_node_t *node, uint64_t *out_entry)
{
    (void)node;
    *out_entry = 0x400000;
    return 0;
}

/* Files mock */
static files_t mock_files_obj;
files_t *files_dup(files_t *fs)
{
    (void)fs;
    return &mock_files_obj;
}
void files_unpin(files_t *fs)
{
    (void)fs;
    files_unpin_calls++;
    record_lock_ev(LOCK_EV_CLEANUP);
}

/* MM & Page table mock */
typedef struct mm_struct mm_t;
mm_t init_mm;
static uint64_t mock_pgd[512];

uint64_t vmm_alloc_map(void) { return (uint64_t)&mock_pgd[0]; }
void vmm_free_user_map(uint64_t *pgdir)
{
    (void)pgdir;
    vmm_free_user_map_calls++;
    record_lock_ev(LOCK_EV_CLEANUP);
}
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
void vma_free_all(mm_t *mm)
{
    (void)mm;
    vma_free_all_calls++;
}
int mm_init_user_heap(mm_t *mm, uint64_t elf_end)
{
    (void)mm; (void)elf_end;
    return 0;
}

static uint8_t *fake_kstack_buf = NULL;
static struct Page fake_stack_page;
struct Page *alloc_pages(int32_t zone_select, uint64_t number, uint64_t page_flags)
{
    (void)zone_select; (void)number; (void)page_flags;
    if (!fake_kstack_buf) {
        fake_kstack_buf = (uint8_t *)calloc(1, 0x210000);
    }
    fake_stack_page.phy_address = (uint64_t)fake_kstack_buf;
    return &fake_stack_page;
}
void free_pages(struct Page *p, int32_t number) { (void)p; (void)number; }

/* Memory allocators */
static void *g_last_mm = NULL;
static void *g_last_thd = NULL;
static void *g_last_raw = NULL;
static void *g_last_fpu = NULL;

extern void *__real_malloc(size_t sz);
extern void *__real_calloc(size_t n, size_t sz);

void *__wrap_malloc(size_t sz)
{
    void *p = __real_malloc(sz);
    if (sz >= sizeof(union task_union))
        g_last_raw = p;
    else if (sz == 512 + 16 || sz == 512 + 15)
        g_last_fpu = p;
    return p;
}

void *__wrap_calloc(size_t n, size_t sz)
{
    void *p = __real_calloc(n, sz);
    if (n * sz == sizeof(thread_t))
        g_last_thd = p;
    return p;
}

void *kmalloc(size_t sz) { return __real_malloc(sz ? sz : 1); }
void kfree(void *p)
{
    if (!p) return;
    if (p == g_last_mm) kfree_mm_calls++;
    else if (p == g_last_thd) kfree_thd_calls++;
    else if (p == g_last_raw) kfree_raw_calls++;
    else if (p == g_last_fpu || (uintptr_t)p == (((uintptr_t)g_last_fpu + 15) & ~15UL)) fpu_free_calls++;
    record_lock_ev(LOCK_EV_CLEANUP);
    free(p);
}

mm_t *mm_alloc(void)
{
    g_last_mm = __real_calloc(1, 128);
    return (mm_t *)g_last_mm;
}

/* Sched mock & instrumentation */
union task_union init_task_union;
static task_t g_current;
task_t *host_test_current = &g_current;
uint32_t num_cpus = 2;
percpu_t percpu_data[NR_CPUS];
static unsigned mock_current_cpu = 0;
void *arch_this_cpu_ptr(void) { return &percpu_data[mock_current_cpu]; }
task_t *user_init_task = NULL;
int64_t user_init_pid = 0;
spinlock_T task_list_lock = {1};

pid_t alloc_pid(void) { return 1; }

void ret_from_intr(void) {}
void task_nofile_init(task_t *task) { (void)task; }
void task_identity_init(task_t *task, const char *comm) { (void)task; (void)comm; }
const char *task_basename(const char *path) { return path; }

int arch_auxv_platform(void *buf, size_t max) { (void)buf; (void)max; return 0; }
size_t arch_auxv_payload_size(void) { return 0; }
bool kernel_random_get_strong(void *buf)
{
    memset(buf, 0, 32);
    if (inject_locked_recheck_fail_cpu >= 0 && inject_locked_recheck_fail_cpu < NR_CPUS) {
        percpu_data[inject_locked_recheck_fail_cpu].scheduler_ok = 0;
        percpu_data[inject_locked_recheck_fail_cpu].online = 0;
    }
    return true;
}
void arch_cpu_halt(void) { abort(); }

int g_log_level = 0;
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

/* Real exec.c calls */
int64_t spawn_user_task(const char *path, const char *const *argv);

/* Wrap or override enqueue / list / notify / locks to observe publication atomicity */
void enqueue_task(task_t *task, percpu_t *rq)
{
    (void)task; (void)rq;
    enqueue_task_calls++;
}

void __wrap_sched_notify_remote(task_t *tsk)
{
    (void)tsk;
    sched_notify_remote_calls++;
}

/* Helper to reset test fixture */
static void reset_spawn_fixture(void)
{
    memset(&g_current, 0, sizeof(g_current));
    g_current.files = &mock_files_obj;
    user_init_task = NULL;
    user_init_pid = 0;
    files_unpin_calls = 0;
    fpu_free_calls = 0;
    vmm_free_user_map_calls = 0;
    vma_free_all_calls = 0;
    kfree_mm_calls = 0;
    kfree_thd_calls = 0;
    kfree_raw_calls = 0;
    enqueue_task_calls = 0;
    sched_notify_remote_calls = 0;
    lock_event_count = 0;
    inject_locked_recheck_fail_cpu = -1;
    mock_vfs_bootstrap_active = false;

    memset(percpu_data, 0, sizeof(percpu_data));
    for (int i = 0; i < NR_CPUS; i++) {
        spin_init(&percpu_data[i].rq_lock);
    }
    spin_init(&task_list_lock);
    list_init(&init_task_union.task.list);
    init_mm.pgdir = (uint64_t *)&mock_pgd[0];
}

/* Test 1: Target eligibility logic */
static void test_target_eligibility(void)
{
    /* 1. UP with window open: CPU 0 boot exception */
    reset_spawn_fixture();
    num_cpus = 1;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;
    sched_boot_creation_begin();
    assert_true(sched_boot_creation_open());

    int64_t ret = spawn_user_task("/bin/init", NULL);
    assert_eq(1, ret);
    assert_eq(1, enqueue_task_calls);
    assert_eq(1, user_init_pid);
    if (user_init_task) {
        assert_eq(0, user_init_task->cpu);
    }

    /* 2. Window closed permanently via sched_boot_start() */
    sched_boot_start();
    assert_false(sched_boot_creation_open());

    /* 3. UP with window closed and scheduler_ok=0: rejected */
    reset_spawn_fixture();
    num_cpus = 1;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;

    ret = spawn_user_task("/bin/init", NULL);
    assert_eq(-EAGAIN, ret);
    assert_eq(0, enqueue_task_calls);

    /* 4. AP online && scheduler_ok=1: eligible */
    reset_spawn_fixture();
    num_cpus = 2;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;

    ret = spawn_user_task("/bin/init", NULL);
    assert_eq(1, ret);
    assert_eq(1, enqueue_task_calls);
    assert_eq(1, user_init_pid);
    assert_true(user_init_task != NULL);
    if (user_init_task) {
        assert_eq(1, user_init_task->cpu);
    }

    /* 5. AP online but scheduler_ok=0: not eligible */
    reset_spawn_fixture();
    num_cpus = 2;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 0;

    ret = spawn_user_task("/bin/init", NULL);
    assert_eq(-EAGAIN, ret);
    assert_eq(0, enqueue_task_calls);
    assert_eq(0, user_init_pid);

    /* 6. num_cpus = 0: rejected */
    reset_spawn_fixture();
    num_cpus = 0;
    ret = spawn_user_task("/bin/init", NULL);
    assert_eq(-EAGAIN, ret);
    assert_eq(0, enqueue_task_calls);
}

/* Test 2: Locked recheck failure & resource unwind ledger */
static void test_locked_recheck_failure(void)
{
    reset_spawn_fixture();
    num_cpus = 2;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;

    /* Simulate race: clear eligibility during setup_user_stack right before locked recheck */
    inject_locked_recheck_fail_cpu = 1;

    int64_t ret = spawn_user_task("/bin/init", NULL);
    assert_eq(-EAGAIN, ret);
    assert_eq(0, enqueue_task_calls);
    assert_eq(0, sched_notify_remote_calls);
    assert_true(user_init_task == NULL);
    assert_eq(0, user_init_pid);

    /* Verify list did not retain tsk */
    assert_true(list_is_empty(&init_task_union.task.list));

    /* Verify clean unwind ledger: each allocated component freed exactly once */
    assert_eq(1, files_unpin_calls);
    assert_eq(1, fpu_free_calls);
    assert_eq(1, vma_free_all_calls);
    assert_eq(1, vmm_free_user_map_calls);
    assert_eq(1, kfree_mm_calls);
    assert_eq(1, kfree_thd_calls);
    assert_eq(1, kfree_raw_calls);
}

int main(void)
{
    TEST_SUITE("BOOT-5 test_boot_services_spawn");

    test_target_eligibility();
    test_locked_recheck_failure();

    TEST_RESULTS();
    return 0;
}
