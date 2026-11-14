#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/user_vm_policy.h>
/* kernel/selftest/test_aarch64_user_fork_cow.c
 *
 * M4 fork + COW runtime acceptance case.
 *
 * One real EL0 parent forks a real child through the native SYS_fork.
 * Both sides run the same fixture image and synchronise only through a
 * kernel-maintained monotonic stage (``svc #0x4d43``), never through
 * page contents.  The case proves, in one single-core boot:
 *
 *   - the child's first frame resumes at the parent's next SVC with
 *     the inherited SP and an independent root, x0 == 0 in the child
 *     and the real child PID in the parent;
 *   - the parent's write to a shared page performs a real COW copy
 *     while the child still holds a reference, and the child still
 *     reads the pre-fork value (data isolation);
 *   - a lazily filled page is allocated independently on each side;
 *   - the stash round trip, the heap-tail privatization on brk shrink
 *     and the clock_gettime write-back COW all keep working after a
 *     fork;
 *   - after the parent munmaps a page, the child's write upgrades the
 *     unique reference in place at the same PA;
 *   - the parent exits first, the child observes PPID == the BSP idle
 *     task and then execs through the inherited catalog, releasing the
 *     last counted page;
 *   - a second short fork's child exits immediately and the parent
 *     confirms the reaped child and its own readable shared page;
 *   - an illegal read-only write exits with -EFAULT through the COW
 *     refusal path;
 *   - every exit is observed at the switch tail by PID, the user root
 *     is revoked and the global 4 KiB page count returns to baseline.
 *
 * The whole TU is test-variant only.
 */

#if AARCH64_EL0_TEST

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_gate.h>
#include <memory/vmm.h>
#include <arch/mmu.h>
#include <kernel.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/user_vm.h>
#include <percpu/percpu.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_user_fork_cow.h>
#include <time/timer.h>
#include <uapi/syscall.h>

extern const char _binary_aarch64_user_fork_cow_main_elf_start[];
extern const char _binary_aarch64_user_fork_cow_main_elf_end[];
extern const char _binary_aarch64_user_fork_cow_cleanup_elf_start[];
extern const char _binary_aarch64_user_fork_cow_cleanup_elf_end[];
extern const char _binary_aarch64_user_fork_cow_illegal_elf_start[];
extern const char _binary_aarch64_user_fork_cow_illegal_elf_end[];
extern const char _binary_aarch64_user_fork_cow_short_elf_start[];
extern const char _binary_aarch64_user_fork_cow_short_elf_end[];

/* The fixture layout (must match aarch64_user_fork_cow.S). */
#define FC_VA_A UINT64_C(0x40000000)
#define FC_VA_B (FC_VA_A + 0x1000)
#define FC_VA_C (FC_VA_A + 0x2000)
#define FC_VA_D (FC_VA_A + 0x3000)
#define FC_VA_E (FC_VA_A + 0x4000)
#define FC_VA_F (FC_VA_A + 0x5000)

#define FC_STATUS_PARENT  (UINT64_C(0x11) << 8)
#define FC_STATUS_CLEANUP (UINT64_C(0x33) << 8)
#define FC_STATUS_SHORT_P (UINT64_C(0x22) << 8)
#define FC_STATUS_SHORT_C (UINT64_C(0x44) << 8)
#define FC_STATUS_FAULT   ((uint64_t)-EFAULT)

/* ── Observation state ────────────────────────────────────────── */

static volatile uint64_t g_stage;
static volatile int      g_fail;
static const char       *g_fail_reason;

static int g_parent_pid = -1;
static int g_child_pid  = -1;
static int g_short_parent_pid = -1;
static int g_short_child_pid  = -1;
static int g_illegal_pid = -1;

static volatile bool g_parent_reaped, g_child_reaped;
static volatile bool g_short_parent_reaped, g_short_child_reaped;
static volatile bool g_illegal_reaped;
static volatile uint64_t g_parent_status, g_child_status;
static volatile uint64_t g_short_parent_status, g_short_child_status;
static volatile uint64_t g_illegal_status;

static bool     g_child_frame_checked;
static bool     g_parent_A_checked, g_parent_F_checked, g_child_B_checked;
static bool     g_child_D_checked, g_E_checked, g_ppid_checked;
static bool     g_parent_ppid_checked;
static uint64_t g_fork_elr, g_fork_sp, g_parent_root;
static uint64_t g_child_expected_elr;
static uint64_t g_parent_A_pa, g_parent_F_pa, g_parent_E_pa;
static uint64_t g_child_B_pa, g_child_D_pa;

static void fc_fail(const char *why)
{
    if (!g_fail) {
        g_fail = 1;
        g_fail_reason = why;
    }
}

static void fc_stage(uint64_t s)
{
    if (s > g_stage)
        g_stage = s;
}

void aarch64_user_fork_cow_test_reset(void)
{
    g_stage = 0;
    g_fail = 0;
    g_fail_reason = NULL;
    g_parent_pid = g_child_pid = -1;
    g_short_parent_pid = g_short_child_pid = -1;
    g_illegal_pid = -1;
    g_parent_reaped = g_child_reaped = false;
    g_short_parent_reaped = g_short_child_reaped = false;
    g_illegal_reaped = false;
    g_parent_status = g_child_status = 0;
    g_short_parent_status = g_short_child_status = 0;
    g_illegal_status = 0;
    g_child_frame_checked = false;
    g_parent_A_checked = g_parent_F_checked = g_child_B_checked = false;
    g_child_D_checked = g_E_checked = g_ppid_checked = false;
    g_parent_ppid_checked = false;
    g_fork_elr = g_fork_sp = g_parent_root = 0;
    g_child_expected_elr = 0;
    g_parent_A_pa = g_parent_F_pa = g_parent_E_pa = 0;
    g_child_B_pa = g_child_D_pa = 0;
}

static uint64_t *fc_root(mm_t *mm)
{
    return (uint64_t *)Phy_To_Virt((uintptr_t)mm->pgdir);
}

static user_vm_page_t *fc_record(mm_t *mm, uint64_t va)
{
    for (list_t *p = mm->resident_list.next; p; p = p->next) {
        user_vm_page_t *r = container_of(p, user_vm_page_t, list);
        if (r->va == va)
            return r;
    }
    return NULL;
}

static int fc_task_pid(void)
{
    task_t *t = current;
    return t ? (int)t->pid : -1;
}

/* ── Production-side observation points ───────────────────────── */

void aarch64_user_fork_cow_test_syscall_enter(struct pt_regs *regs,
                                              uint64_t nr)
{
    int pid = fc_task_pid();

    if (nr == SYS_fork && pid == g_parent_pid) {
        g_fork_elr = regs->elr_el1;
        g_fork_sp  = regs->sp_el0;
        return;
    }
    /* The parent's getpid is the first instruction pair after the fork
     * return.  Both sides execute it from the same inherited frame, so
     * the child's first native SVC must report the same ELR and SP. */
    if (nr == SYS_getpid && pid == g_parent_pid) {
        g_child_expected_elr = regs->elr_el1;
        g_fork_sp = regs->sp_el0;
        return;
    }

    if (pid != g_child_pid || g_child_frame_checked)
        return;

    g_child_frame_checked = true;

    uint64_t expected = g_child_expected_elr
                            ? g_child_expected_elr
                            : g_fork_elr + 12;   /* fallback */
    if (regs->elr_el1 != expected) {
        log_err("[fork-cow] child elr=%lx expected=%lx sp=%lx fsp=%lx\n",
                (unsigned long)regs->elr_el1, (unsigned long)expected,
                (unsigned long)regs->sp_el0, (unsigned long)g_fork_sp);
        fc_fail("the child resumed at the wrong PC");
    }
    if (regs->sp_el0 != g_fork_sp)
        fc_fail("the child inherited a different SP");

    mm_t *mm = aarch64_user_current_mm();
    if (!mm || !mm->pgdir) {
        fc_fail("the child has no mm");
        return;
    }
    if ((uint64_t)(uintptr_t)mm->pgdir == g_parent_root)
        fc_fail("the child shares the parent root");

    uint64_t *root = fc_root(mm);
    struct user_vm_leaf leaf;
    if (arch_user_vm_query(root, FC_VA_B, &leaf) != 0 ||
        leaf.state != USER_VM_PRESENT)
        fc_fail("child B is not present");
    else
        g_child_B_pa = leaf.pa;
    if (arch_user_vm_query(root, FC_VA_D, &leaf) != 0 ||
        leaf.state != USER_VM_PRESENT)
        fc_fail("child D is not present");
    else
        g_child_D_pa = leaf.pa;

    fc_stage(2);
}

void aarch64_user_fork_cow_test_syscall_ret(struct pt_regs *regs,
                                            uint64_t nr, int64_t rc)
{
    (void)regs;
    int pid = fc_task_pid();

    if (nr == SYS_fork) {
        if (pid == g_parent_pid && rc > 0) {
            g_child_pid = (int)rc;
            mm_t *mm = aarch64_user_current_mm();
            g_parent_root = mm ? (uint64_t)(uintptr_t)mm->pgdir : 0;

            struct user_vm_leaf leaf;
            uint64_t *root = mm ? fc_root(mm) : NULL;
            if (!root ||
                arch_user_vm_query(root, FC_VA_A, &leaf) != 0 ||
                leaf.state != USER_VM_PRESENT)
                fc_fail("parent A is not present after fork");
            else
                g_parent_A_pa = leaf.pa;
            if (!root ||
                arch_user_vm_query(root, FC_VA_F, &leaf) != 0 ||
                leaf.state != USER_VM_PRESENT)
                fc_fail("parent F is not present after fork");
            else
                g_parent_F_pa = leaf.pa;
            fc_stage(1);
        } else if (pid == g_short_parent_pid && rc > 0) {
            g_short_child_pid = (int)rc;
        }
        return;
    }

    if (pid == g_child_pid) {
        if (nr == SYS_clock_gettime && rc == 0) {
            mm_t *mm = aarch64_user_current_mm();
            user_vm_page_t *r = mm ? fc_record(mm, FC_VA_B) : NULL;
            if (!r || r->share != USER_VM_PAGE_PRIVATE)
                fc_fail("child B was not privatized");
            else if (r->pa == g_child_B_pa)
                fc_fail("child B kept the shared PA");
            else if (page_cow_refs(g_child_B_pa) != 1)
                fc_fail("child B's old PA lost the parent reference");
            else
                g_child_B_checked = true;
            fc_stage(3);
        }
        if (nr == SYS_getppid) {
            if (!g_parent_reaped) {
                /* While the parent is alive, getppid must name the
                 * real parent task, not the idle task. */
                if ((int)rc != g_parent_pid)
                    fc_fail("the child PPID is not the live parent");
                else
                    g_parent_ppid_checked = true;
            } else {
                int idle = percpu_data[0].idle
                               ? (int)percpu_data[0].idle->pid : -1;
                if ((int)rc != idle)
                    fc_fail("the child PPID is not the BSP idle task");
                else
                    g_ppid_checked = true;
                fc_stage(7);
            }
        }
    }

    if (pid == g_parent_pid) {
        if (nr == SYS_clock_gettime && g_stage >= 2) {
            mm_t *mm = aarch64_user_current_mm();
            user_vm_page_t *r = mm ? fc_record(mm, FC_VA_F) : NULL;
            if (!r || r->share != USER_VM_PAGE_PRIVATE)
                fc_fail("parent F was not privatized");
            else if (r->pa == g_parent_F_pa)
                fc_fail("parent F kept the shared PA");
            else if (page_cow_refs(g_parent_F_pa) != 1)
                fc_fail("parent F's old PA lost the child reference");
            else
                g_parent_F_checked = true;
        }
        if (nr == SYS_munmap) {
            if (g_child_D_pa && page_cow_refs(g_child_D_pa) != 1)
                fc_fail("D did not drop to the child's single reference");
            fc_stage(4);
        }
    }
}

void aarch64_user_fork_cow_test_fault(struct pt_regs *regs, uint64_t esr,
                                      uint64_t far, int rc)
{
    (void)regs;
    if (rc != 0)
        return;

    int pid = fc_task_pid();
    uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;
    uint64_t va  = far & ~((uint64_t)PAGE_4K_SIZE - 1);
    mm_t *mm = aarch64_user_current_mm();
    if (!mm)
        return;

    if (fsc >= AARCH64_FSC_TRANSLATION_FIRST &&
        fsc <= AARCH64_FSC_TRANSLATION_LAST) {
        struct user_vm_leaf leaf;
        if (pid == g_parent_pid && va == FC_VA_E) {
            if (arch_user_vm_query(fc_root(mm), va, &leaf) != 0 ||
                leaf.state != USER_VM_PRESENT)
                fc_fail("parent E was not filled");
            else
                g_parent_E_pa = leaf.pa;
        } else if (pid == g_child_pid && va == FC_VA_E) {
            if (arch_user_vm_query(fc_root(mm), va, &leaf) != 0 ||
                leaf.state != USER_VM_PRESENT)
                fc_fail("child E was not filled");
            else if (!g_parent_E_pa || leaf.pa == g_parent_E_pa)
                fc_fail("the lazy E page was not allocated independently");
            else
                g_E_checked = true;
        }
        return;
    }

    if (fsc != 0x0fULL)
        return;

    if (pid == g_parent_pid && va == FC_VA_A) {
        user_vm_page_t *r = fc_record(mm, va);
        struct user_vm_leaf leaf;
        if (!r || r->share != USER_VM_PAGE_PRIVATE)
            fc_fail("parent A was not privatized");
        else if (arch_user_vm_query(fc_root(mm), va, &leaf) != 0 ||
                 leaf.state != USER_VM_PRESENT || leaf.pa == g_parent_A_pa)
            fc_fail("parent A did not copy to a new PA");
        else if (page_cow_refs(g_parent_A_pa) != 1)
            fc_fail("parent A's old PA lost the child reference");
        else
            g_parent_A_checked = true;
    } else if (pid == g_child_pid && va == FC_VA_D) {
        user_vm_page_t *r = fc_record(mm, va);
        struct user_vm_leaf leaf;
        if (!r || r->share != USER_VM_PAGE_PRIVATE)
            fc_fail("child D was not upgraded to exclusive ownership");
        else if (arch_user_vm_query(fc_root(mm), va, &leaf) != 0 ||
                 leaf.state != USER_VM_PRESENT || leaf.pa != g_child_D_pa)
            fc_fail("child D changed PA on the unique upgrade");
        else if (leaf.flags != (VM_USER_RW | VM_NO_EXEC))
            fc_fail("child D is not writable after the upgrade");
        else if (page_cow_refs(leaf.pa) != 0)
            fc_fail("child D kept a reference after the upgrade");
        else
            g_child_D_checked = true;
        fc_stage(5);
    }
}

void aarch64_user_fork_cow_test_reap(struct task_struct *prev)
{
    /* The switch-tail observer runs for every switched-out task; only
     * a PF_SELF_REAP task has reached its exit tail and a meaningful
     * exit_code.  A mid-run yield must never look like a reap. */
    if (!prev || !(prev->flags & PF_SELF_REAP))
        return;

    int pid = (int)prev->pid;
    uint64_t status = (uint64_t)prev->exit_code;

    if (pid == g_parent_pid) {
        g_parent_reaped = true;
        g_parent_status = status;
        fc_stage(6);
    } else if (g_child_pid >= 0 && pid == g_child_pid) {
        g_child_reaped = true;
        g_child_status = status;
    } else if (pid == g_short_parent_pid) {
        g_short_parent_reaped = true;
        g_short_parent_status = status;
    } else if (g_short_child_pid >= 0 && pid == g_short_child_pid) {
        g_short_child_reaped = true;
        g_short_child_status = status;
        fc_stage(1);
    } else if (pid == g_illegal_pid) {
        g_illegal_reaped = true;
        g_illegal_status = status;
    }
}

int64_t aarch64_user_fork_cow_test_stage_query(void)
{
    int pid = fc_task_pid();
    if (pid == g_parent_pid || pid == g_child_pid ||
        pid == g_short_parent_pid || pid == g_short_child_pid)
        return (int64_t)g_stage;
    return -1;
}

/* ── The case body ────────────────────────────────────────────── */

struct fc_run {
    int rc;
    uint64_t pmm_before;
    uint64_t pmm_after;
};

static void fc_reap_observe(task_t *prev)
{
    aarch64_user_fork_cow_test_reap(prev);
}

static void fc_wait(bool *first, bool *second, uint64_t deadline)
{
    while ((!*first || !*second) && jiffies < deadline)
        kthread_yield();
}

static uint64_t fc_worker(uint64_t arg)
{
    struct fc_run *run = (struct fc_run *)(uintptr_t)arg;

    const struct aarch64_user_elf_source main_catalog[2] = {
        { _binary_aarch64_user_fork_cow_cleanup_elf_start,
          (size_t)(_binary_aarch64_user_fork_cow_cleanup_elf_end -
                   _binary_aarch64_user_fork_cow_cleanup_elf_start) },
        { _binary_aarch64_user_fork_cow_main_elf_start,
          (size_t)(_binary_aarch64_user_fork_cow_main_elf_end -
                   _binary_aarch64_user_fork_cow_main_elf_start) },
    };
    const struct aarch64_user_elf_source illegal_catalog[1] = {
        { _binary_aarch64_user_fork_cow_illegal_elf_start,
          (size_t)(_binary_aarch64_user_fork_cow_illegal_elf_end -
                   _binary_aarch64_user_fork_cow_illegal_elf_start) },
    };
    const struct aarch64_user_elf_source short_catalog[1] = {
        { _binary_aarch64_user_fork_cow_short_elf_start,
          (size_t)(_binary_aarch64_user_fork_cow_short_elf_end -
                   _binary_aarch64_user_fork_cow_short_elf_start) },
    };

    aarch64_user_fork_cow_test_reset();
    aarch64_sched_set_reap_observer(fc_reap_observe);

    run->pmm_before = pmm_probe_4k_allocated_count();

    /* ── Case 1: the full parent/child fork ───────────────────── */
    int pid = aarch64_user_task_spawn(main_catalog, 2, 1, "fork_cow");
    if (pid < 0) {
        fc_fail("spawning the fork fixture failed");
        goto done;
    }
    g_parent_pid = pid;

    {
        uint64_t deadline = jiffies + 4000;
        while ((!g_parent_reaped || !g_child_reaped) && !g_fail &&
               jiffies < deadline)
            kthread_yield();
    }
    if (!g_parent_reaped || !g_child_reaped) {
        log_err("[fork-cow] reaped parent=%d child=%d\n",
                (int)g_parent_reaped, (int)g_child_reaped);
        fc_fail("the fork case did not finish in time");
    } else if (g_parent_status != FC_STATUS_PARENT) {
        log_err("[fork-cow] parent status=%lx expected=%lx stage=%lx\n",
                (unsigned long)g_parent_status,
                (unsigned long)FC_STATUS_PARENT, (unsigned long)g_stage);
        fc_fail("the parent exit status is wrong");
    } else if (g_child_status != FC_STATUS_CLEANUP) {
        log_err("[fork-cow] child status=%lx expected=%lx stage=%lx\n",
                (unsigned long)g_child_status,
                (unsigned long)FC_STATUS_CLEANUP, (unsigned long)g_stage);
        fc_fail("the exec'd child exit status is wrong");
    }

    if (!g_child_frame_checked)
        fc_fail("the child frame was never observed");
    if (!g_parent_A_checked)
        fc_fail("the parent's multi-reference COW was not observed");
    if (!g_parent_F_checked)
        fc_fail("the parent's clock_gettime COW was not observed");
    if (!g_child_B_checked)
        fc_fail("the child's clock_gettime COW was not observed");
    if (!g_child_D_checked)
        fc_fail("the child's unique-reference upgrade was not observed");
    if (!g_E_checked)
        fc_fail("the independent lazy page was not observed");
    if (!g_parent_ppid_checked)
        fc_fail("the child's live-parent PPID was not observed");
    if (!g_ppid_checked)
        fc_fail("the child's idle PPID was not observed");
    if (g_stage < 7)
        fc_fail("the child did not reach the post-exec stage");
    if (g_child_pid <= 0)
        fc_fail("the parent never learned the child PID");

    g_child_pid = -1;   /* the short case must not match this PID */

    /* ── Case 2: a second short fork ──────────────────────────── */
    g_stage = 0;
    g_short_parent_pid = g_short_child_pid = -1;
    g_short_parent_reaped = g_short_child_reaped = false;
    g_short_parent_status = g_short_child_status = 0;

    pid = aarch64_user_task_spawn(short_catalog, 1, 0, "fork_short");
    if (pid < 0) {
        fc_fail("spawning the short fork fixture failed");
        goto done;
    }
    g_short_parent_pid = pid;

    {
        uint64_t deadline = jiffies + 2000;
        while ((!g_short_parent_reaped || !g_short_child_reaped) && !g_fail &&
               jiffies < deadline)
            kthread_yield();
    }
    if (!g_short_parent_reaped || !g_short_child_reaped)
        fc_fail("the short fork case did not finish in time");
    else if (g_short_parent_status != FC_STATUS_SHORT_P)
        fc_fail("the short-fork parent exit status is wrong");
    else if (g_short_child_status != FC_STATUS_SHORT_C)
        fc_fail("the short-fork child exit status is wrong");

    g_short_parent_pid = g_short_child_pid = -1;

    /* ── Case 3: the illegal read-only write ──────────────────── */
    g_illegal_pid = -1;
    g_illegal_reaped = false;
    g_illegal_status = 0;

    pid = aarch64_user_task_spawn(illegal_catalog, 1, 0, "fork_illegal");
    if (pid < 0) {
        fc_fail("spawning the illegal-write fixture failed");
        goto done;
    }
    g_illegal_pid = pid;

    {
        uint64_t deadline = jiffies + 2000;
        while (!g_illegal_reaped && !g_fail && jiffies < deadline)
            kthread_yield();
    }
    if (!g_illegal_reaped)
        fc_fail("the illegal-write case did not finish in time");
    else if (g_illegal_status != FC_STATUS_FAULT)
        fc_fail("the illegal write did not exit with -EFAULT");

    /* ── Global baseline ──────────────────────────────────────── */
    run->pmm_after = pmm_probe_4k_allocated_count();
    if (run->pmm_after != run->pmm_before)
        fc_fail("the 4 KiB page count did not return to its baseline");
    else if (aarch64_pt_published_root_count() != 1)
        fc_fail("a user root stayed published");

done:
    aarch64_sched_set_reap_observer(NULL);
    if (g_fail)
        log_err("[fork-cow] FAIL: %s\n",
                g_fail_reason ? g_fail_reason : "(unknown)");
    run->rc = g_fail ? 1 : 0;
    return 0;
}

static volatile bool g_done;
static struct fc_run  g_run;

static uint64_t fc_worker_entry(uint64_t arg)
{
    uint64_t rc = fc_worker(arg);
    g_done = true;
    return rc;
}

int test_aarch64_user_fork_cow(void)
{
    memset(&g_run, 0, sizeof(g_run));
    g_run.rc = 1;
    g_done = false;

    kernel_thread_on_cpu(fc_worker_entry, (uintptr_t)&g_run, 0);

    uint64_t deadline = jiffies + 8000;
    while (!g_done && jiffies < deadline)
        kthread_yield();

    return g_done ? g_run.rc : 1;
}

#endif /* AARCH64_EL0_TEST */
