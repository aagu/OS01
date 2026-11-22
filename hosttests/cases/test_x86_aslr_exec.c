/* @os01-test
 * id: test_x86_aslr_exec
 * layer: host
 * subsystem: sched
 * timing: strict
 * tier: pr
 * isolation: none
 * scenarios: [memory.aslr.x86_exec]
 */
/*
 * Host tests for x86_64 spawn_user_task / sys_exec ASLR integration:
 * independent STRONG-only entropy calls for mmap_base and AT_RANDOM,
 * fail-closed -EAGAIN rollback on either RNG call, and old_mm preservation
 * without arch_switch_mm on sys_exec failure.
 */
#include "test_framework.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <arch/x86_64/aslr.h>
#include <arch/x86_64/elf_image.h>
#include <sys/auxv.h>
#include <arch/x86_64/mmu.h>
#include <arch/x86_64/user_layout.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <memory/mm.h>
#include <memory/pmm.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>

/* ── Production entry points under test ── */
int64_t spawn_user_task(const char *path, const char *const *argv);
int64_t sys_exec(const char *path, pt_regs_t *regs,
                 const char *const *argv, const char *const *envp);

/* ── Allocation & rollback ledger ── */
extern void *__real_malloc(size_t sz);
extern void *__real_calloc(size_t n, size_t sz);

static void *g_last_mm = NULL;
static void *g_old_mm = NULL;
static void *g_last_thd = NULL;
static void *g_last_raw = NULL;
static void *g_last_fpu = NULL;

static int kfree_mm_calls = 0;
static int kfree_old_mm_calls = 0;
static int kfree_thd_calls = 0;
static int kfree_raw_calls = 0;
static int fpu_free_calls = 0;

static int files_unpin_calls = 0;
static int vma_free_all_calls = 0;
static int vmm_free_user_map_calls = 0;
static int alloc_pages_calls = 0;
static int enqueue_calls = 0;
static int task_comm_apply_exec_calls = 0;
static int switch_mm_calls = 0;
static uint64_t *switch_mm_last_pgdir = NULL;

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
    if (n * sz == sizeof(mm_t))
        g_last_mm = p;
    else if (n * sz == sizeof(thread_t))
        g_last_thd = p;
    return p;
}

void *kmalloc(size_t sz)
{
    return __real_malloc(sz ? sz : 1);
}

size_t kfree(void *p)
{
    if (!p)
        return 0;
    if (p == g_old_mm) {
        kfree_old_mm_calls++;
        g_old_mm = NULL;
    } else if (p == g_last_mm) {
        kfree_mm_calls++;
        g_last_mm = NULL;
    } else if (p == g_last_thd) {
        kfree_thd_calls++;
        g_last_thd = NULL;
    } else if (p == g_last_raw) {
        kfree_raw_calls++;
        g_last_raw = NULL;
    } else if (p == g_last_fpu ||
               (uintptr_t)p == (((uintptr_t)g_last_fpu + 15UL) & ~15UL)) {
        fpu_free_calls++;
        g_last_fpu = NULL;
    }
    free(p);
    return 0;
}

/* ── Controllable STRONG RNG sequence stub ── */
#define RNG_MAX_SEQ 4
static int g_rng_calls = 0;
static bool g_rng_seq_ok[RNG_MAX_SEQ];
static uint64_t g_rng_seq_u64[RNG_MAX_SEQ];

static void reset_rng_seq(bool ok0, uint64_t val0, bool ok1, uint64_t val1)
{
    g_rng_calls = 0;
    g_rng_seq_ok[0] = ok0;
    g_rng_seq_u64[0] = val0;
    g_rng_seq_ok[1] = ok1;
    g_rng_seq_u64[1] = val1;
    for (int i = 2; i < RNG_MAX_SEQ; i++) {
        g_rng_seq_ok[i] = true;
        g_rng_seq_u64[i] = val1 + (uint64_t)i;
    }
}

bool kernel_random_get_strong(uint8_t out[32])
{
    int idx = g_rng_calls++;
    if (idx >= RNG_MAX_SEQ)
        idx = RNG_MAX_SEQ - 1;
    uint64_t v = g_rng_seq_u64[idx];
    if (out) {
        memset(out, 0xA5, 32);
        for (int i = 0; i < 8; i++)
            out[i] = (uint8_t)((v >> (i * 8)) & 0xFFU);
    }
    return g_rng_seq_ok[idx];
}

/* ── Kernel globals & stubs for exec.c ── */
static uint8_t g_init_pgd[4096] __attribute__((aligned(4096)));
mm_t init_mm = { .pgdir = (uint64_t *)g_init_pgd };

union task_union init_task_union;
static task_t g_current;
static thread_t g_current_thread;
task_t *host_test_current = &g_current;

uint32_t num_cpus = 1;
percpu_t percpu_data[NR_CPUS];
task_t *user_init_task = NULL;
int64_t user_init_pid = 0;
spinlock_T task_list_lock = { 1 };

static files_t g_fake_files;
static vfs_node_t g_fake_node;
static struct Page g_fake_stack_page;
static void *g_stack_backing = NULL;
static void *g_user_pgd_backing = NULL;

void aslr_exec_record_switch_mm(uint64_t *pgdir)
{
    switch_mm_calls++;
    switch_mm_last_pgdir = pgdir;
}

void *arch_this_cpu_ptr(void) { return &percpu_data[0]; }
pid_t alloc_pid(void) { return 42; }
bool vfs_bootstrap_active(void) { return false; }
unsigned sched_pick_cpu_mask(uint64_t mask) { (void)mask; return 0; }
bool sched_boot_creation_open(void) { return true; }
void sched_notify_remote(task_t *tsk) { (void)tsk; }
void arch_cpu_halt(void) { abort(); }

void enqueue_task(task_t *task, percpu_t *rq)
{
    (void)task;
    (void)rq;
    enqueue_calls++;
}

vfs_node_t *vfs_lookup(const char *path)
{
    (void)path;
    return &g_fake_node;
}

int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node)
{
    (void)dirfd;
    (void)path;
    (void)flags;
    if (out_node)
        *out_node = &g_fake_node;
    return 0;
}

void vfs_node_put(vfs_node_t *node) { (void)node; }

static int g_mock_elf_rc = 0;
static bool g_mock_elf_is_pie = false;
static uint64_t g_mock_elf_entry = 0x401000UL;
static uint64_t g_mock_elf_phdr = 0;
static uint64_t g_mock_elf_phnum = 0;

int elf_validate(vfs_node_t *node) { (void)node; return 0; }
uint32_t elf_detect_abi(vfs_node_t *node) { (void)node; return 0; }

int elf_load(vfs_node_t *node, mm_t *mm, x86_elf_image_t *out_image)
{
    (void)node;
    if (g_mock_elf_rc != 0)
        return g_mock_elf_rc;

    uint64_t bias = 0;
    if (g_mock_elf_is_pie) {
        int brc = x86_aslr_get_pie_bias(&bias);
        if (brc != 0)
            return brc;
    }

    mm->start_code = g_mock_elf_is_pie ? bias : X86_USER_MIN_ADDR;
    mm->end_code = g_mock_elf_is_pie ? (bias + 0x10000UL) : 0x500000UL;
    if (out_image) {
        out_image->entry = g_mock_elf_is_pie ? (bias + g_mock_elf_entry) : g_mock_elf_entry;
        out_image->phdr = g_mock_elf_is_pie ? (bias + g_mock_elf_phdr) : 0;
        out_image->phnum = g_mock_elf_phnum;
        out_image->is_pie = g_mock_elf_is_pie;
    }
    return 0;
}

int mm_init_user_heap(mm_t *mm, uint64_t brk)
{
    mm->start_brk = brk;
    mm->end_brk = brk;
    return 0;
}

uint64_t *vmm_alloc_map(void)
{
    void *p = __real_calloc(1, 4096);
    g_user_pgd_backing = p;
    return (uint64_t *)p;
}

void vmm_free_user_map(uint64_t *pgd)
{
    vmm_free_user_map_calls++;
    if (pgd && pgd == g_user_pgd_backing)
        g_user_pgd_backing = NULL;
    free(pgd);
}

void vma_free_all(mm_t *mm)
{
    (void)mm;
    vma_free_all_calls++;
}

struct Page *alloc_pages(int32_t zone_select, uint64_t number, uint64_t page_flags)
{
    (void)zone_select;
    (void)number;
    (void)page_flags;
    alloc_pages_calls++;
    if (!g_stack_backing)
        g_stack_backing = __real_calloc(1, 0x210000);
    else
        memset(g_stack_backing, 0, 0x210000);
    g_fake_stack_page.phy_address = (uint64_t)(uintptr_t)g_stack_backing;
    return &g_fake_stack_page;
}

void x86_vmm_map_2m(uint64_t *pgdir, uint64_t phy_addr, uint64_t virt_addr, uint64_t flags)
{
    (void)pgdir;
    (void)phy_addr;
    (void)virt_addr;
    (void)flags;
}

files_t *files_dup(files_t *src) { return src; }
void files_unpin(files_t *f) { (void)f; files_unpin_calls++; }
void task_nofile_init(task_t *task) { (void)task; }
void task_identity_init(task_t *task, const char *comm) { (void)task; (void)comm; }
const char *task_basename(const char *path) { return path; }

int task_comm_apply_exec(task_t *task, const char *path)
{
    (void)task;
    (void)path;
    task_comm_apply_exec_calls++;
    return 0;
}

void arch_task_prepare_user_entry(task_t *task, struct pt_regs *frame)
{
    (void)task;
    (void)frame;
}

const char *arch_auxv_platform(void)
{
    return "x86_64";
}

size_t arch_auxv_payload_size(void) { return 7; }

int g_log_level = 0;
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

static void reset_exec_fixture(void)
{
    memset(&init_task_union, 0, sizeof(init_task_union));
    list_init(&init_task_union.task.list);
    spin_init(&task_list_lock);
    memset(percpu_data, 0, sizeof(percpu_data));
    num_cpus = 1;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 1;
    spin_init(&percpu_data[0].rq_lock);

    memset(&g_current, 0, sizeof(g_current));
    memset(&g_current_thread, 0, sizeof(g_current_thread));
    memset(&g_fake_files, 0, sizeof(g_fake_files));
    memset(&g_fake_node, 0, sizeof(g_fake_node));
    g_fake_node.type = VFS_FILE;
    g_current.files = &g_fake_files;
    g_current.thread = &g_current_thread;

    user_init_task = NULL;
    user_init_pid = 0;

    g_last_mm = NULL;
    g_old_mm = NULL;
    g_last_thd = NULL;
    g_last_raw = NULL;
    g_last_fpu = NULL;

    kfree_mm_calls = 0;
    kfree_old_mm_calls = 0;
    kfree_thd_calls = 0;
    kfree_raw_calls = 0;
    fpu_free_calls = 0;
    files_unpin_calls = 0;
    vma_free_all_calls = 0;
    vmm_free_user_map_calls = 0;
    alloc_pages_calls = 0;
    enqueue_calls = 0;
    task_comm_apply_exec_calls = 0;
    switch_mm_calls = 0;
    switch_mm_last_pgdir = NULL;
    g_mock_elf_rc = 0;
    g_mock_elf_is_pie = false;
    g_mock_elf_entry = 0x401000UL;
    g_mock_elf_phdr = 0;
    g_mock_elf_phnum = 0;
}

/* ── Tests ── */

TEST_FUNC(test_spawn_aslr_strong_success)
{
    reset_exec_fixture();
    uint64_t r1 = 0x0000000012345678ULL;
    uint64_t r2 = 0x0000000087654321ULL;
    reset_rng_seq(true, r1, true, r2);

    const char *argv[] = { "/bin/init", NULL };
    int64_t pid = spawn_user_task("/bin/init", argv);
    assert_true(pid > 0);
    assert_eq(2, g_rng_calls);
    assert_not_null(user_init_task);
    assert_eq(x86_aslr_derive_mmap_base(r1), user_init_task->mm->mmap_base);
    assert_eq(X86_USER_ADDR_LIMIT, user_init_task->addr_limit);
}

TEST_FUNC(test_spawn_aslr_entropy_fail_closed)
{
    reset_exec_fixture();
    reset_rng_seq(false, 0x11111111ULL, true, 0x22222222ULL);

    const char *argv[] = { "/bin/init", NULL };
    int64_t pid = spawn_user_task("/bin/init", argv);
    assert_eq(-EAGAIN, pid);
    assert_eq(0, alloc_pages_calls);
    assert_eq(0, enqueue_calls);
    assert_null(user_init_task);

    assert_eq(1, files_unpin_calls);
    assert_eq(1, fpu_free_calls);
    assert_eq(1, vma_free_all_calls);
    assert_eq(1, vmm_free_user_map_calls);
    assert_eq(1, kfree_mm_calls);
    assert_eq(1, kfree_thd_calls);
    assert_eq(1, kfree_raw_calls);
}

TEST_FUNC(test_spawn_at_random_fail_after_aslr)
{
    reset_exec_fixture();
    reset_rng_seq(true, 0x0000000012345678ULL, false, 0x0ULL);

    const char *argv[] = { "/bin/init", NULL };
    int64_t pid = spawn_user_task("/bin/init", argv);
    assert_eq(-EAGAIN, pid);
    assert_eq(0, enqueue_calls);
    assert_null(user_init_task);

    assert_eq(1, files_unpin_calls);
    assert_eq(1, fpu_free_calls);
    assert_eq(1, vma_free_all_calls);
    assert_eq(1, vmm_free_user_map_calls);
    assert_eq(1, kfree_mm_calls);
    assert_eq(1, kfree_thd_calls);
    assert_eq(1, kfree_raw_calls);
}

TEST_FUNC(test_exec_aslr_strong_success)
{
    reset_exec_fixture();
    mm_t *old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
    old_mm->mmap_base = 0x40000000ULL;
    old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
    g_old_mm = old_mm;
    g_current.mm = old_mm;

    uint64_t r1 = 0x000000000abcdef1ULL;
    uint64_t r2 = 0x000000000fedcba2ULL;
    reset_rng_seq(true, r1, true, r2);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    const char *argv[] = { "/bin/sh", NULL };
    const char *envp[] = { NULL };

    int64_t rc = sys_exec("/bin/sh", &regs, argv, envp);
    assert_eq(0, rc);
    assert_eq(2, g_rng_calls);
    assert_true(g_current.mm != old_mm);
    assert_eq(x86_aslr_derive_mmap_base(r1), g_current.mm->mmap_base);
    assert_eq(1, switch_mm_calls);
    assert_true(switch_mm_last_pgdir == g_current.mm->pgdir);
    assert_eq(1, vma_free_all_calls);
    assert_eq(1, vmm_free_user_map_calls);
    assert_eq(1, kfree_old_mm_calls);
}

TEST_FUNC(test_exec_aslr_entropy_fail_preserves_old_mm)
{
    reset_exec_fixture();
    mm_t *old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
    old_mm->mmap_base = 0x0000401234500000ULL;
    old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
    g_old_mm = old_mm;
    g_current.mm = old_mm;

    reset_rng_seq(false, 0ULL, true, 0x22222222ULL);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    const char *argv[] = { "/bin/sh", NULL };
    const char *envp[] = { NULL };

    int64_t rc = sys_exec("/bin/sh", &regs, argv, envp);
    assert_eq(-EAGAIN, rc);
    assert_eq(0, switch_mm_calls);
    assert_true(g_current.mm == old_mm);
    assert_eq(0x0000401234500000ULL, old_mm->mmap_base);
    assert_eq(0, task_comm_apply_exec_calls);
    assert_eq(1, kfree_mm_calls);
    assert_eq(0, kfree_old_mm_calls);

    free(old_mm->pgdir);
    free(old_mm);
}

TEST_FUNC(test_exec_at_random_fail_preserves_old_mm)
{
    reset_exec_fixture();
    mm_t *old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
    old_mm->mmap_base = 0x000040abcdef0000ULL;
    old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
    g_old_mm = old_mm;
    g_current.mm = old_mm;

    reset_rng_seq(true, 0x0000000011223344ULL, false, 0ULL);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    const char *argv[] = { "/bin/sh", NULL };
    const char *envp[] = { NULL };

    int64_t rc = sys_exec("/bin/sh", &regs, argv, envp);
    assert_eq(-EAGAIN, rc);
    assert_eq(0, switch_mm_calls);
    assert_true(g_current.mm == old_mm);
    assert_eq(0x000040abcdef0000ULL, old_mm->mmap_base);
    assert_eq(0, task_comm_apply_exec_calls);
    assert_eq(1, kfree_mm_calls);
    assert_eq(0, kfree_old_mm_calls);

    free(old_mm->pgdir);
    free(old_mm);
}

static void reset_rng_seq3(bool ok0, uint64_t val0,
                           bool ok1, uint64_t val1,
                           bool ok2, uint64_t val2)
{
    g_rng_calls = 0;
    g_rng_seq_ok[0] = ok0; g_rng_seq_u64[0] = val0;
    g_rng_seq_ok[1] = ok1; g_rng_seq_u64[1] = val1;
    g_rng_seq_ok[2] = ok2; g_rng_seq_u64[2] = val2;
    g_rng_seq_ok[3] = true; g_rng_seq_u64[3] = val2 + 1;
}

static const void *stack_va_ptr(uint64_t va)
{
    return (const void *)((const uint8_t *)g_stack_backing + (va - X86_USER_STACK_BASE));
}

static uint64_t stack_read_u64(uint64_t va)
{
    uint64_t v = 0;
    memcpy(&v, stack_va_ptr(va), sizeof(v));
    return v;
}

static uint64_t g_prepared_rsp = 0;
static uint64_t g_prepared_rip = 0;

static void verify_stack_auxv(uint64_t rsp, int exp_argc, bool is_pie,
                              uint64_t exp_entry, uint64_t exp_phdr,
                              uint64_t exp_phnum, uint64_t exp_rng_u64)
{
    assert_eq(0ULL, rsp & 0xFULL);
    uint64_t argc = stack_read_u64(rsp);
    assert_eq((uint64_t)exp_argc, argc);

    uint64_t p = rsp + 8;
    for (int i = 0; i < exp_argc; i++) {
        assert_true(stack_read_u64(p) != 0);
        p += 8;
    }
    assert_eq(0ULL, stack_read_u64(p)); /* argv NULL */
    p += 8;
    while (stack_read_u64(p) != 0)
        p += 8;
    p += 8; /* envp NULL */

    bool seen_phdr = false, seen_phent = false, seen_phnum = false;
    bool seen_entry = false, seen_base = false, seen_pagesz = false;
    bool seen_plat = false, seen_rand = false, seen_null = false;
    int pairs = 0;

    for (int i = 0; i < 16; i++) {
        uint64_t tag = stack_read_u64(p);
        uint64_t val = stack_read_u64(p + 8);
        p += 16;
        pairs++;
        if (tag == AT_NULL) {
            seen_null = true;
            assert_eq(0ULL, val);
            break;
        }
        if (tag == AT_PHDR) { seen_phdr = true; assert_eq(exp_phdr, val); }
        else if (tag == AT_PHENT) { seen_phent = true; assert_eq(56ULL, val); }
        else if (tag == AT_PHNUM) { seen_phnum = true; assert_eq(exp_phnum, val); }
        else if (tag == AT_ENTRY) { seen_entry = true; assert_eq(exp_entry, val); }
        else if (tag == AT_BASE) { seen_base = true; assert_eq(0ULL, val); }
        else if (tag == AT_PAGESZ) { seen_pagesz = true; assert_eq(4096ULL, val); }
        else if (tag == AT_PLATFORM) { seen_plat = true; assert_true(val >= X86_USER_STACK_BASE); }
        else if (tag == AT_RANDOM) {
            seen_rand = true;
            uint64_t rand_first_u64 = stack_read_u64(val);
            assert_eq(exp_rng_u64, rand_first_u64);
        }
    }

    assert_true(seen_null && seen_plat && seen_rand);
    if (is_pie) {
        assert_eq(9, pairs);
        assert_true(seen_phdr && seen_phent && seen_phnum &&
                    seen_entry && seen_base && seen_pagesz);
    } else {
        assert_eq(3, pairs);
        assert_false(seen_phdr || seen_phent || seen_phnum ||
                     seen_entry || seen_base || seen_pagesz);
    }
}

TEST_FUNC(test_pie_spawn_and_exec_auxv_3rng_success)
{
    /* Intercept arch_task_prepare_user_entry via g_prepared_*? Let's check exec.c
     * sets regs->rsp/rip for sys_exec directly, so test sys_exec for full ABI. */
    reset_exec_fixture();
    g_mock_elf_is_pie = true;
    g_mock_elf_entry = 0x1234ULL;
    g_mock_elf_phdr = 0x40ULL;
    g_mock_elf_phnum = 4ULL;

    mm_t *old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
    old_mm->mmap_base = 0x40000000ULL;
    old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
    g_old_mm = old_mm;
    g_current.mm = old_mm;

    uint64_t r0 = 0x0002ULL; /* PIE bias = 0x402000 */
    uint64_t r1 = 0x0000000012345678ULL; /* mmap_base */
    uint64_t r2 = 0xCAFEBABEDEADBEEFULL; /* AT_RANDOM */
    reset_rng_seq3(true, r0, true, r1, true, r2);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    const char *argv[] = { "/bin/pie", "arg1", NULL };
    const char *envp[] = { "A=1", NULL };

    int64_t rc = sys_exec("/bin/pie", &regs, argv, envp);
    assert_eq(0, rc);
    assert_eq(3, g_rng_calls);
    uint64_t B = X86_USER_PIE_BIAS_BASE + ((r0 & 0xFFFFULL) << 12);
    assert_eq(B + 0x1234ULL, regs.rip);
    verify_stack_auxv(regs.rsp, 2, true, B + 0x1234ULL, B + 0x40ULL, 4ULL, r2);

    /* Now spawn_user_task with PIE */
    reset_exec_fixture();
    g_mock_elf_is_pie = true;
    g_mock_elf_entry = 0x2000ULL;
    g_mock_elf_phdr = 0x40ULL;
    g_mock_elf_phnum = 3ULL;
    reset_rng_seq3(true, r0, true, r1, true, r2);
    int64_t pid = spawn_user_task("/bin/pie", argv);
    assert_true(pid > 0);
    assert_eq(3, g_rng_calls);
    (void)g_prepared_rsp;
    (void)g_prepared_rip;

    /* ET_EXEC sys_exec: 2 RNG calls and 3 auxv pairs */
    reset_exec_fixture();
    old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
    old_mm->mmap_base = 0x40000000ULL;
    old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
    g_old_mm = old_mm;
    g_current.mm = old_mm;
    reset_rng_seq3(true, r1, true, r2, true, 0);
    memset(&regs, 0, sizeof(regs));
    rc = sys_exec("/bin/exec", &regs, argv, envp);
    assert_eq(0, rc);
    assert_eq(2, g_rng_calls);
    assert_eq(0x401000ULL, regs.rip);
    verify_stack_auxv(regs.rsp, 2, false, 0, 0, 0, r2);
}

TEST_FUNC(test_pie_three_stage_eagain_and_error_propagation)
{
    const char *argv[] = { "/bin/pie", NULL };
    const char *envp[] = { NULL };

    /* 3 stages of -EAGAIN on spawn_user_task */
    for (int stage = 0; stage < 3; stage++) {
        reset_exec_fixture();
        g_mock_elf_is_pie = true;
        reset_rng_seq3(stage != 0, 1, stage != 1, 2, stage != 2, 3);
        int64_t pid = spawn_user_task("/bin/pie", argv);
        assert_eq(-EAGAIN, pid);
        assert_eq(stage + 1, g_rng_calls);
        assert_eq(0, enqueue_calls);
        assert_null(user_init_task);
        assert_eq(1, files_unpin_calls);
        assert_eq(1, fpu_free_calls);
        assert_eq(1, vmm_free_user_map_calls);
        assert_eq(1, kfree_mm_calls);
        assert_eq(1, kfree_thd_calls);
        assert_eq(1, kfree_raw_calls);
    }

    /* 3 stages of -EAGAIN on sys_exec */
    for (int stage = 0; stage < 3; stage++) {
        reset_exec_fixture();
        g_mock_elf_is_pie = true;
        mm_t *old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
        old_mm->mmap_base = 0x0000401234000000ULL;
        old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
        g_old_mm = old_mm;
        g_current.mm = old_mm;
        reset_rng_seq3(stage != 0, 1, stage != 1, 2, stage != 2, 3);

        pt_regs_t regs;
        memset(&regs, 0, sizeof(regs));
        int64_t rc = sys_exec("/bin/pie", &regs, argv, envp);
        assert_eq(-EAGAIN, rc);
        assert_eq(stage + 1, g_rng_calls);
        assert_eq(0, switch_mm_calls);
        assert_true(g_current.mm == old_mm);
        assert_eq(0x0000401234000000ULL, old_mm->mmap_base);
        assert_eq(1, kfree_mm_calls);
        assert_eq(0, kfree_old_mm_calls);
        free(old_mm->pgdir);
        free(old_mm);
    }

    /* Propagate -ENOEXEC and -ENOMEM from elf_load */
    int errs[2] = { -ENOEXEC, -ENOMEM };
    for (int i = 0; i < 2; i++) {
        reset_exec_fixture();
        g_mock_elf_rc = errs[i];
        assert_eq(errs[i], spawn_user_task("/bin/bad", argv));

        reset_exec_fixture();
        g_mock_elf_rc = errs[i];
        mm_t *old_mm = (mm_t *)__real_calloc(1, sizeof(mm_t));
        old_mm->pgdir = (uint64_t *)__real_calloc(1, 4096);
        g_old_mm = old_mm;
        g_current.mm = old_mm;
        pt_regs_t regs;
        memset(&regs, 0, sizeof(regs));
        assert_eq(errs[i], sys_exec("/bin/bad", &regs, argv, envp));
        assert_eq(0, switch_mm_calls);
        free(old_mm->pgdir);
        free(old_mm);
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_spawn_aslr_strong_success),
    TEST_ENTRY(test_spawn_aslr_entropy_fail_closed),
    TEST_ENTRY(test_spawn_at_random_fail_after_aslr),
    TEST_ENTRY(test_exec_aslr_strong_success),
    TEST_ENTRY(test_exec_aslr_entropy_fail_preserves_old_mm),
    TEST_ENTRY(test_exec_at_random_fail_preserves_old_mm),
    TEST_ENTRY(test_pie_spawn_and_exec_auxv_3rng_success),
    TEST_ENTRY(test_pie_three_stage_eagain_and_error_propagation),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return 0;
}
