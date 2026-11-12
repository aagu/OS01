/* kernel/selftest/test_aarch64_user_vm.c
 *
 * M4 anonymous-VM acceptance case.
 *
 * Runs the six fixture images one at a time and proves, in the SAME
 * boot:
 *
 *   - mmap/brk commit metadata only (no leaf right after the call);
 *   - a first touch fills a zero page; PROT_NONE keeps and restores
 *     the old data; an inner mprotect splits the VMA;
 *   - a range spanning a hole instantiates nothing, and a resident
 *     prefix is not copied on -EFAULT (both clock_gettime shapes);
 *   - a brk shrink zeroes the retained tail and a re-grow does not
 *     reallocate it;
 *   - permission, PROT_NONE and unmapped accesses all terminate the
 *     task with -EFAULT through the old fault path;
 *   - an injected replacement failure leaves the old root, VMA and
 *     data intact; restoring the stash reads the original data; a
 *     later replacement succeeds;
 *   - munmap, exit and successful replacement each release their
 *     stashed page, so the 4 KiB page count and the published-root
 *     count return to their baselines.
 *
 * The whole TU is test-variant only.
 */

#if AARCH64_EL0_TEST

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <arch/aarch64/user_task.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/mmu.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/user_vm.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_user_task.h>
#include <selftest/aarch64_user_vm.h>
#include <syscall/dispatch.h>
#include <time/timer.h>
#include <uapi/syscall.h>

extern const char _binary_aarch64_user_vm_normal_elf_start[];
extern const char _binary_aarch64_user_vm_normal_elf_end[];
extern const char _binary_aarch64_user_vm_rw_only_elf_start[];
extern const char _binary_aarch64_user_vm_rw_only_elf_end[];
extern const char _binary_aarch64_user_vm_none_elf_start[];
extern const char _binary_aarch64_user_vm_none_elf_end[];
extern const char _binary_aarch64_user_vm_unmapped_elf_start[];
extern const char _binary_aarch64_user_vm_unmapped_elf_end[];
extern const char _binary_aarch64_user_vm_replace_elf_start[];
extern const char _binary_aarch64_user_vm_replace_elf_end[];
extern const char _binary_aarch64_user_vm_leave_elf_start[];
extern const char _binary_aarch64_user_vm_leave_elf_end[];

#define VM_HEAP_BASE      UINT64_C(0x00402000)
#define VM_CLOCK_SENTINEL UINT64_C(0xdeadbeefdeadbeef)

#define VM_STATUS_NORMAL  (UINT64_C(0x11) << 8)
#define VM_STATUS_REPLACE (UINT64_C(0x33) << 8)
#define VM_STATUS_FAULT   ((uint64_t)-EFAULT)

/* ── Observation state (production hooks + this case) ─────────── */

static struct aarch64_user_vm_stats g_stats;
static bool g_watch_clock;
static int  g_arm_pid = -1;
static bool g_brk_checked;

void aarch64_user_vm_test_reset(void)
{
    g_stats = (struct aarch64_user_vm_stats){0};
    g_watch_clock = false;
    g_arm_pid = -1;
    g_brk_checked = false;
}

void aarch64_user_vm_test_stats(struct aarch64_user_vm_stats *out)
{
    if (out)
        *out = g_stats;
}

void aarch64_user_vm_test_watch_clock(bool on)
{
    g_watch_clock = on;
}

void aarch64_user_vm_test_arm_replace_fail(int pid)
{
    g_arm_pid = pid;
}

static uint64_t *vm_root_of(mm_t *mm)
{
    return (uint64_t *)Phy_To_Virt((uintptr_t)mm->pgdir);
}

/* Native SVC return observation. */
void aarch64_user_vm_test_syscall(struct pt_regs *regs, uint64_t nr,
                                  int64_t rc)
{
    g_stats.syscalls++;

    mm_t *mm = aarch64_user_current_mm();
    if (!mm || !mm->pgdir)
        return;
    uint64_t *root = vm_root_of(mm);

    if (nr == SYS_mmap) {
        g_stats.mmap_calls++;
        if (rc >= 0 && ((uint64_t)rc & (PAGE_4K_SIZE - 1)) == 0) {
            struct user_vm_leaf leaf;
            if (arch_user_vm_query(root, (uint64_t)rc, &leaf) == 0 &&
                leaf.state != USER_VM_ABSENT)
                g_stats.mmap_absent_violations++;
        }
    } else if (nr == SYS_brk) {
        g_stats.brk_calls++;
        if (rc > 0 && !g_brk_checked) {
            /* The first growing brk: the page holding the new break
             * must still be absent. */
            g_brk_checked = true;
            uint64_t va = ((uint64_t)rc - 1) & ~((uint64_t)PAGE_4K_SIZE - 1);
            struct user_vm_leaf leaf;
            if (arch_user_vm_query(root, va, &leaf) == 0 &&
                leaf.state != USER_VM_ABSENT)
                g_stats.brk_absent_violations++;
            g_stats.brk_absent_checked++;
        }
    } else if (nr == SYS_clock_gettime && rc == -EFAULT &&
               g_watch_clock && regs && regs->x19) {
        uint64_t va = regs->x19 & ~((uint64_t)PAGE_4K_SIZE - 1);
        struct user_vm_leaf leaf;
        if (arch_user_vm_query(root, va, &leaf) == 0 &&
            g_stats.clock_samples < AARCH64_USER_VM_CLOCK_SAMPLES) {
            unsigned i = g_stats.clock_samples++;
            g_stats.clock[i].va = va;
            g_stats.clock[i].state = (int)leaf.state;
            g_stats.clock[i].sentinel =
                leaf.state == USER_VM_PRESENT
                    ? *(volatile uint64_t *)((unsigned char *)
                                             Phy_To_Virt(leaf.pa) +
                                             (PAGE_4K_SIZE - 8))
                    : 0;
            g_stats.clock_errors++;
        }
    }

    /* The replacement fixture's first VM syscall arms the one-shot
     * image failure, so the exec itself sees -ENOMEM. */
    if (g_arm_pid >= 0 && current && (int)current->pid == g_arm_pid &&
        (nr == SYS_mmap || nr == SYS_brk || nr == SYS_mprotect ||
         nr == SYS_munmap)) {
        aarch64_user_image_test_arm_fail(g_arm_pid);
        g_arm_pid = -1;
    }
}

void aarch64_user_vm_test_fault(uint64_t esr, uint64_t far, uint64_t pc,
                                int rc)
{
    g_stats.faults++;
    g_stats.last_fault_esr = esr;
    g_stats.last_fault_far = far;
    g_stats.last_fault_pc = pc;
    g_stats.last_fault_rc = rc;
    if (rc == 0)
        g_stats.fault_retried++;
    else if (rc == -ENOMEM)
        g_stats.fault_oom++;
    else
        g_stats.fault_refused++;
}

void aarch64_user_vm_test_exec(int rc, uint64_t old_root_pa)
{
    g_stats.execs++;
    if (g_stats.exec_records < AARCH64_USER_VM_EXEC_RECORDS) {
        unsigned i = g_stats.exec_records++;
        g_stats.exec[i].rc = rc;
        g_stats.exec[i].old_root_pa = old_root_pa;
    }
}

/* ── Reap observation ─────────────────────────────────────────── */

static volatile int      g_reaped_pid;
static volatile uint64_t g_reaped_status;
static volatile bool     g_reaped;

static void vm_reap_observe(task_t *prev)
{
    if (!prev)
        return;
    g_reaped_pid = (int)prev->pid;
    g_reaped_status = (uint64_t)prev->exit_code;
    g_reaped = true;
}

/* ── The case body ────────────────────────────────────────────── */

enum {
    FIX_NORMAL,
    FIX_RW_ONLY,
    FIX_NONE,
    FIX_UNMAPPED,
    FIX_REPLACE,
    FIX_COUNT,
};

struct vm_fixture {
    const char *label;
    const char *start;
    const char *end;
    uint64_t    expected_status;
    bool        normal;
    bool        replace;
    /* Replacement target (catalog index 0); the fixture itself is
     * index 1 when a target exists. */
    const char *target_start;
    const char *target_end;
};

struct vm_run {
    int rc;
    struct aarch64_user_vm_stats stats[FIX_COUNT];
    uint64_t pmm_before;
    uint64_t pmm_after;
};

static int vm_fail(struct vm_run *run, const char *what)
{
    log_err("[user-vm] FAIL: %s\n", what);
    run->rc = 1;
    return 1;
}

static int vm_run_one(struct vm_run *run, unsigned index,
                      const struct vm_fixture *fx)
{
    size_t size = (size_t)(fx->end - fx->start);
    if (!size)
        return vm_fail(run, "the fixture image is empty");

    aarch64_user_vm_test_reset();
    aarch64_user_vm_test_watch_clock(fx->normal);
    g_reaped = false;
    g_reaped_pid = -1;
    g_reaped_status = 0;

    struct aarch64_user_elf_source catalog[2];
    size_t catalog_count = 1;
    size_t catalog_index = 0;
    if (fx->target_start) {
        catalog[0] = (struct aarch64_user_elf_source){
            fx->target_start,
            (size_t)(fx->target_end - fx->target_start) };
        catalog[1] = (struct aarch64_user_elf_source){ fx->start, size };
        catalog_count = 2;
        catalog_index = 1;
    } else {
        catalog[0] = (struct aarch64_user_elf_source){ fx->start, size };
    }

    aarch64_sched_set_reap_observer(vm_reap_observe);
    int pid = aarch64_user_task_spawn(catalog, catalog_count,
                                      catalog_index, fx->label);
    if (pid < 0) {
        aarch64_sched_set_reap_observer(NULL);
        return vm_fail(run, "spawning the fixture task failed");
    }
    if (fx->replace)
        aarch64_user_vm_test_arm_replace_fail(pid);

    {
        uint64_t deadline = jiffies + 500;
        while (!g_reaped && jiffies < deadline)
            kthread_yield();
    }
    aarch64_sched_set_reap_observer(NULL);

    if (!g_reaped || g_reaped_pid != pid)
        return vm_fail(run, "the fixture task did not exit in time");
    if (g_reaped_status != fx->expected_status) {
        log_err("[user-vm] %s exit=%lx expected=%lx\n", fx->label,
                (unsigned long)g_reaped_status,
                (unsigned long)fx->expected_status);
        return vm_fail(run, "the fixture exited with the wrong status");
    }

    aarch64_user_vm_test_stats(&run->stats[index]);
    return 0;
}

static int vm_check_normal(struct vm_run *run, const struct vm_fixture *fx)
{
    const struct aarch64_user_vm_stats *s = &run->stats[FIX_NORMAL];
    if (s->mmap_absent_violations)
        return vm_fail(run, "mmap instantiated a leaf");
    if (s->brk_absent_checked != 1 || s->brk_absent_violations)
        return vm_fail(run, "brk growth instantiated a leaf");
    if (s->fault_retried < 1)
        return vm_fail(run, "no demand fault was retried");
    if (s->clock_samples != 2 || s->clock_errors != 2)
        return vm_fail(run, "the two clock_gettime probes were not observed");
    if (s->clock[0].state != USER_VM_ABSENT)
        return vm_fail(run, "the hole probe instantiated its first leaf");
    if (s->clock[1].state != USER_VM_PRESENT)
        return vm_fail(run, "the resident probe lost its leaf");
    if (s->clock[1].sentinel != VM_CLOCK_SENTINEL)
        return vm_fail(run, "the -EFAULT copy changed the resident sentinel");
    (void)fx;
    return 0;
}

static int vm_check_fault_fixture(struct vm_run *run, unsigned index,
                                  uint64_t fsc_first, uint64_t fsc_last,
                                  const char *what)
{
    const struct aarch64_user_vm_stats *s = &run->stats[index];
    uint64_t ec = (s->last_fault_esr >> 26) & 0x3f;
    uint64_t fsc = s->last_fault_esr & 0x3f;

    /* One demand fault fills the page, then the illegal access is the
     * only refused fault and the last one observed. */
    if (s->fault_refused != 1 || s->fault_retried < 1 ||
        s->last_fault_rc != -EFAULT)
        return vm_fail(run, "the fault did not take the termination path");
    if (ec != 0x24)
        return vm_fail(run, "the fault was not an EL0 Data Abort");
    if (fsc < fsc_first || fsc > fsc_last) {
        log_err("[user-vm] %s fsc=%lx\n", what, (unsigned long)fsc);
        return vm_fail(run, "the fault syndrome class is wrong");
    }
    return 0;
}

static int vm_check_replace(struct vm_run *run)
{
    const struct aarch64_user_vm_stats *s = &run->stats[FIX_REPLACE];
    if (s->exec_records != 2)
        return vm_fail(run, "the replacement was not attempted twice");
    if (s->exec[0].rc != -ENOMEM)
        return vm_fail(run, "the injected replacement did not fail -ENOMEM");
    if (s->exec[1].rc != 0)
        return vm_fail(run, "the second replacement did not succeed");
    if (!s->exec[0].old_root_pa ||
        s->exec[0].old_root_pa != s->exec[1].old_root_pa)
        return vm_fail(run, "the old root changed across the replacement");
    return 0;
}

static uint64_t vm_worker(uint64_t arg)
{
    struct vm_run *run = (struct vm_run *)(uintptr_t)arg;

    static const struct vm_fixture fixtures[FIX_COUNT] = {
        { "user_vm_normal", _binary_aarch64_user_vm_normal_elf_start,
          _binary_aarch64_user_vm_normal_elf_end, VM_STATUS_NORMAL,
          true, false, NULL, NULL },
        { "user_vm_rw", _binary_aarch64_user_vm_rw_only_elf_start,
          _binary_aarch64_user_vm_rw_only_elf_end, VM_STATUS_FAULT,
          false, false, NULL, NULL },
        { "user_vm_none", _binary_aarch64_user_vm_none_elf_start,
          _binary_aarch64_user_vm_none_elf_end, VM_STATUS_FAULT,
          false, false, NULL, NULL },
        { "user_vm_unmapped", _binary_aarch64_user_vm_unmapped_elf_start,
          _binary_aarch64_user_vm_unmapped_elf_end, VM_STATUS_FAULT,
          false, false, NULL, NULL },
        { "user_vm_replace", _binary_aarch64_user_vm_replace_elf_start,
          _binary_aarch64_user_vm_replace_elf_end, VM_STATUS_REPLACE,
          false, true,
          _binary_aarch64_user_vm_leave_elf_start,
          _binary_aarch64_user_vm_leave_elf_end },
    };

    run->pmm_before = pmm_probe_4k_allocated_count();

    for (unsigned i = 0; i < FIX_COUNT; i++) {
        if (vm_run_one(run, i, &fixtures[i]))
            goto out;
    }

    if (vm_check_normal(run, &fixtures[FIX_NORMAL]))
        goto out;
    if (vm_check_fault_fixture(run, FIX_RW_ONLY, 0x0c, 0x0f, "rw_only"))
        goto out;
    if (vm_check_fault_fixture(run, FIX_NONE, 0x04, 0x07, "none"))
        goto out;
    if (vm_check_fault_fixture(run, FIX_UNMAPPED, 0x04, 0x07, "unmapped"))
        goto out;
    if (vm_check_replace(run))
        goto out;

    run->pmm_after = pmm_probe_4k_allocated_count();
    if (run->pmm_after != run->pmm_before)
        vm_fail(run, "the 4 KiB page count did not return to its baseline");
    else if (aarch64_pt_published_root_count() != 1)
        vm_fail(run, "a user root stayed published");

out:
    return 0;
}

int test_aarch64_user_vm(void)
{
    struct vm_run run = {0};
    kernel_thread_on_cpu(vm_worker, (uintptr_t)&run, 0);
    kthread_yield();
    return run.rc;
}

#endif /* AARCH64_EL0_TEST */
