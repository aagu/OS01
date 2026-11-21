/* kernel/selftest/test_aarch64_user_migration.c — Dual-core EL0 task migration
 * acceptance selftest (USER_RUNTIME_CASE=migration).
 */

#ifdef AARCH64_USER_RUNTIME_CASE_MIGRATION

#include <arch/aarch64/ap_work.h>
#include <arch/cpu.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/sched_tick.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/timer.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_mm.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/irq.h>
#include <arch/mmu.h>
#include <arch/regs.h>
#include <errno.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <kernel.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <percpu/percpu.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_user_runtime.h>
#include <selftest/result.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sync/wait.h>
#include <uapi/syscall.h>

extern const unsigned char _binary_aarch64_user_migration_elf_start[];
extern const unsigned char _binary_aarch64_user_migration_elf_end[];
extern const unsigned char _binary_aarch64_user_migration_exec_elf_start[];
extern const unsigned char _binary_aarch64_user_migration_exec_elf_end[];

extern unsigned aarch64_asid_test_in_use_count(void);
extern void arch_aarch64_idle_step(void);
extern uint32_t aarch64_sched_tick_mig_cadence(uint32_t cpu);
extern bool aarch64_sched_tick_mig_pending(uint32_t cpu);
extern int64_t spawn_user_task(const char *path, const char *const *argv);

#define MIG_PATH_MAIN "/tmp/urt-migration"
#define MIG_PATH_EXEC "/tmp/urt-migration-exec"
#define MIG_PATH_COPY "/tmp/urt-migration-copy"

#define MIG_DATA_VA     0x40000000ULL
#define MIG_COW_VA      0x40001000ULL
#define MIG_CTRL_VA     0x40002000ULL
#define MIG_CTRL_C1     0x00ULL
#define MIG_CTRL_C2     0x08ULL
#define MIG_CTRL_M1_GO  0x20ULL
#define MIG_CTRL_M2_GO  0x28ULL
#define MIG_CTRL_M3_GO  0x30ULL

#define MIG_MAX_EVENTS 256

enum mig_ev_kind {
    ME_NONE = 0,
    ME_BASELINE,
    ME_FINAL,
    ME_SPAWN,
    ME_FORK,
    ME_PARENT_CHECK,
    ME_COW_CHECK,
    ME_OLD_LOAD,
    ME_REMAP,
    ME_NEW_LOAD,
    ME_PERM_ABORT,
    ME_EDIT_BEGIN,
    ME_TLBI_DONE,
    ME_EDIT_LEAF,
    ME_EDIT_VMA,
    ME_FULL_FLUSH,
    ME_TLBI_TAG,
    ME_IRQ_SAVE,
    ME_HW_DETACH,
    ME_ROOT_UNPUBLISH,
    ME_TAIL,
    ME_COMMIT,
    ME_PICK,
    ME_HW_INSTALL,
    ME_IRQ_RESUME,
    ME_EL0_AFTER,
    ME_IDLE_ZERO,
    ME_WFI_ENTER,
    ME_IDLE_TICK,
    ME_IDLE_CHECK,
    ME_IDLE_TAKE,
    ME_BSP_TICK_SCHED,
    ME_COPY_OK,
    ME_COPY_DENIED,
    ME_EXEC_COMMIT,
    ME_EXEC_EL0,
    ME_WAIT_STATUS,
};

struct mig_event {
    uint32_t seq;
    enum mig_ev_kind kind;
    int64_t a, b, c, d, e;
    uint64_t u0, u1, u2, u3;
    char op[16];
};

static struct mig_event g_events[MIG_MAX_EVENTS];
static uint32_t g_mig_seq;
static bool g_mig_overflow;
static volatile bool g_mig_active;
static volatile bool g_mig_done;
static int g_mig_rc;

static int g_parent_pid = -1;
static int g_main_pid = -1;
static int g_child2_pid = -1;
static volatile int g_active_move_id;
static task_t * volatile g_mig_worker_task;
static spinlock_T g_mig_lock = { .lock = 1UL };

static uint32_t g_sgi_cnt[NR_CPUS];
static uint32_t g_other_irq_cnt[NR_CPUS];
static uint32_t g_ap_work_cnt[NR_CPUS];
static uint32_t g_direct_sched_cnt[NR_CPUS];
static uint32_t g_last_irq_intid[NR_CPUS];
static volatile uint32_t g_bsp_tick_req_seen;

static wait_queue_t g_helper_wq[2];
static wait_queue_t g_worker_wq;
static volatile bool g_helper_waiting[2];
static volatile bool g_helper_stop;
static volatile bool g_helper_done[2];

static volatile bool g_move_tail_seen[4];
static volatile bool g_move_committed[4];
static volatile bool g_move_picked[4];
static volatile bool g_move_installed[4];
static volatile bool g_move_resumed[4];
static volatile bool g_move_el0_seen[4];
static volatile bool g_move_window_open[4];
static volatile bool g_move1_wfi_ready;
static volatile int g_move1_tick_idx;
static volatile bool g_move1_took;
static volatile bool g_move_bsp_sched_seen[4];

static volatile uint64_t g_ctrl_pa;
static volatile bool g_parent_checked;
static volatile bool g_cow_checked;
static volatile bool g_old_load_checked;
static volatile bool g_remap_checked;
static volatile bool g_exec_committed;
static volatile bool g_exec_el0_seen;

struct irq_snap {
    bool valid;
    uintptr_t frame_addr;
    pt_regs_t regs;
    uint64_t tpidr;
    uint64_t tpidrro;
};
static struct irq_snap g_irq_snap[4];

static uint32_t g_vm_tx_counter;
static volatile uint32_t g_active_vm_tx;
static struct mig_event * volatile g_active_vm_eb;
static struct mig_event * volatile g_active_vm_td;
static volatile bool g_active_vm_tlbi_done;
static volatile bool g_vm_window_active;

static void mig_reset_window_counters(uint32_t cpu)
{
    if (cpu < NR_CPUS) {
        __atomic_store_n(&g_sgi_cnt[cpu], 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&g_other_irq_cnt[cpu], 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&g_ap_work_cnt[cpu], 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&g_direct_sched_cnt[cpu], 0U, __ATOMIC_RELEASE);
    }
    if (cpu == 0)
        __atomic_store_n(&g_bsp_tick_req_seen, 0U, __ATOMIC_RELEASE);
}

static inline uint64_t mig_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t mig_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v ? v : 62500000ULL;
}

static struct mig_event *mig_reserve(enum mig_ev_kind kind, uint32_t *seq_out)
{
    uint32_t s = __atomic_fetch_add(&g_mig_seq, 1, __ATOMIC_RELAXED) + 1U;
    if (s > MIG_MAX_EVENTS) {
        g_mig_overflow = true;
        return NULL;
    }
    struct mig_event *ev = &g_events[s - 1U];
    ev->seq = s;
    ev->kind = kind;
    if (seq_out)
        *seq_out = s;
    return ev;
}

static uint64_t mig_task_root_pa(const task_t *tsk)
{
    return (tsk && tsk->mm) ? (uint64_t)(uintptr_t)tsk->mm->pgdir : 0ULL;
}

static uint8_t mig_task_asid(const task_t *tsk)
{
    if (!tsk || !tsk->mm)
        return 0;
    return (uint8_t)(aarch64_user_ttbr0_value(tsk->mm) >> 48);
}

static int mig_move_for_pid(int pid)
{
    int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
    if (mid == 1 && pid == g_main_pid)
        return 1;
    if (mid == 2 && pid == g_main_pid)
        return 2;
    if (mid == 3 && pid == g_child2_pid)
        return 3;
    return 0;
}

static void mig_write_ctrl(uint64_t off, uint64_t val)
{
    uint64_t pa = __atomic_load_n(&g_ctrl_pa, __ATOMIC_ACQUIRE);
    if (!pa)
        return;
    volatile uint64_t *p = (volatile uint64_t *)((uintptr_t)Phy_To_Virt(pa) + off);
    __atomic_store_n(p, val, __ATOMIC_RELEASE);
}

static uint64_t mig_read_ctrl(uint64_t off)
{
    uint64_t pa = __atomic_load_n(&g_ctrl_pa, __ATOMIC_ACQUIRE);
    if (!pa)
        return 0;
    volatile uint64_t *p = (volatile uint64_t *)((uintptr_t)Phy_To_Virt(pa) + off);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static uint64_t mig_read_user_u64(const task_t *tsk, uint64_t va, uint64_t *pa_out)
{
    if (!tsk || !tsk->mm || !tsk->mm->pgdir)
        return 0;
    uint64_t pa = 0;
    uint32_t vm_flags = 0;
    if (arch_vmm_query_4k((uint64_t *)Phy_To_Virt((uint64_t)(uintptr_t)tsk->mm->pgdir),
                          va, &pa, &vm_flags) != 0)
        return 0;
    if (pa_out)
        *pa_out = pa;
    return *(volatile uint64_t *)Phy_To_Virt(pa);
}

bool aarch64_user_runtime_test_migration_veto(const struct task_struct *tsk)
{
    if (!g_mig_active || !tsk)
        return false;
    int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
    if (mid == 1 && (int)tsk->pid == g_main_pid &&
        __atomic_load_n(&g_move_window_open[1], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&g_move1_wfi_ready, __ATOMIC_ACQUIRE))
        return false;
    if (mid == 2 && (int)tsk->pid == g_main_pid &&
        __atomic_load_n(&g_move_window_open[2], __ATOMIC_ACQUIRE))
        return false;
    if (mid == 3 && (int)tsk->pid == g_child2_pid &&
        __atomic_load_n(&g_move_window_open[3], __ATOMIC_ACQUIRE))
        return false;
    return true;
}

void aarch64_user_mig_note_irq_save(const struct task_struct *tsk,
                                    const struct pt_regs *frame)
{
    if (!g_mig_active || !tsk || !frame)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || g_irq_snap[mid].valid)
        return;
    if (!__atomic_load_n(&g_move_window_open[mid], __ATOMIC_ACQUIRE))
        return;
    g_irq_snap[mid].valid = true;
    g_irq_snap[mid].frame_addr = (uintptr_t)frame;
    g_irq_snap[mid].regs = *frame;
    aarch64_user_vm_save_tls(&g_irq_snap[mid].tpidr, &g_irq_snap[mid].tpidrro);

    struct mig_event *ev = mig_reserve(ME_IRQ_SAVE, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu_id();
        ev->u0 = mig_task_root_pa(tsk);
        ev->u1 = mig_task_asid(tsk);
        ev->u2 = (uint64_t)(uintptr_t)frame;
    }
}

void aarch64_user_mig_note_hw_detach(const struct task_struct *tsk,
                                     uint64_t root_pa, uint8_t asid)
{
    if (!g_mig_active || !tsk)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || !g_irq_snap[mid].valid || g_move_tail_seen[mid])
        return;
    uint64_t ttbr0 = 0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    struct mig_event *ev = mig_reserve(ME_HW_DETACH, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu_id();
        ev->u0 = root_pa;
        ev->u1 = asid;
        ev->u2 = ttbr0;
    }
}

void aarch64_user_mig_note_root_unpublish(const struct task_struct *tsk,
                                          uint64_t root_pa, uint8_t asid)
{
    if (!g_mig_active || !tsk)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || !g_irq_snap[mid].valid || g_move_tail_seen[mid])
        return;
    struct mig_event *ev = mig_reserve(ME_ROOT_UNPUBLISH, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu_id();
        ev->u0 = root_pa;
        ev->u1 = asid;
    }
}

void aarch64_user_mig_note_tail(const struct task_struct *tsk)
{
    if (!g_mig_active || !tsk)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || !g_irq_snap[mid].valid || g_move_tail_seen[mid])
        return;
    struct mig_event *ev = mig_reserve(ME_TAIL, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu_id();
        ev->d = __atomic_load_n(&tsk->on_cpu, __ATOMIC_RELAXED);
        ev->u0 = mig_task_root_pa(tsk);
        ev->u1 = mig_task_asid(tsk);
    }
    __atomic_store_n(&g_move_tail_seen[mid], true, __ATOMIC_RELEASE);
}

void aarch64_user_mig_note_commit(const struct task_struct *tsk,
                                  uint32_t src_cpu, uint32_t dst_cpu)
{
    if (!g_mig_active || !tsk)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || g_move_committed[mid])
        return;
    if ((mid == 2 || mid == 3) && dst_cpu == 0 && !g_move_bsp_sched_seen[mid]) {
        g_move_bsp_sched_seen[mid] = true;
        struct mig_event *bev = mig_reserve(ME_BSP_TICK_SCHED, NULL);
        if (bev) {
            bev->a = mid;
            bev->b = dst_cpu;
            bev->c = __atomic_load_n(&g_bsp_tick_req_seen, __ATOMIC_ACQUIRE);
            bev->u0 = __atomic_load_n(&g_sgi_cnt[0], __ATOMIC_ACQUIRE);
            bev->u1 = __atomic_load_n(&g_ap_work_cnt[0], __ATOMIC_ACQUIRE);
            bev->u2 = __atomic_load_n(&g_direct_sched_cnt[0], __ATOMIC_ACQUIRE);
        }
    }
    struct mig_event *ev = mig_reserve(ME_COMMIT, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = dst_cpu;
        ev->d = src_cpu;
        ev->e = dst_cpu;
        ev->u0 = mig_task_root_pa(tsk);
        ev->u1 = mig_task_asid(tsk);
    }
    __atomic_store_n(&g_move_committed[mid], true, __ATOMIC_RELEASE);
}

void aarch64_user_mig_note_pick(const struct task_struct *tsk, uint32_t cpu)
{
    if (!g_mig_active || !tsk)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || !g_move_committed[mid] || g_move_picked[mid])
        return;
    if ((mid == 1 && cpu != 1) || ((mid == 2 || mid == 3) && cpu != 0))
        return;
    struct mig_event *ev = mig_reserve(ME_PICK, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu;
        ev->d = __atomic_load_n(&tsk->on_cpu, __ATOMIC_RELAXED);
        ev->u0 = mig_task_root_pa(tsk);
        ev->u1 = mig_task_asid(tsk);
    }
    __atomic_store_n(&g_move_picked[mid], true, __ATOMIC_RELEASE);
}

void aarch64_user_mig_note_hw_install(const struct task_struct *tsk,
                                      uint64_t root_pa, uint8_t asid)
{
    if (!g_mig_active || !tsk)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || !g_move_picked[mid] || g_move_installed[mid])
        return;
    struct mig_event *ev = mig_reserve(ME_HW_INSTALL, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu_id();
        ev->u0 = root_pa;
        ev->u1 = asid;
    }
    __atomic_store_n(&g_move_installed[mid], true, __ATOMIC_RELEASE);
}

void aarch64_user_mig_note_irq_resume(const struct task_struct *tsk,
                                      const struct pt_regs *frame)
{
    if (!g_mig_active || !tsk || !frame)
        return;
    int mid = mig_move_for_pid((int)tsk->pid);
    if (!mid || !g_move_installed[mid] || g_move_resumed[mid])
        return;
    uint64_t t0 = 0, t1 = 0;
    aarch64_user_vm_save_tls(&t0, &t1);
    bool match = ((uintptr_t)frame == g_irq_snap[mid].frame_addr) &&
                 (__builtin_memcmp(frame, &g_irq_snap[mid].regs, sizeof(*frame)) == 0) &&
                 (t0 == g_irq_snap[mid].tpidr) &&
                 (t1 == g_irq_snap[mid].tpidrro);
    struct mig_event *ev = mig_reserve(ME_IRQ_RESUME, NULL);
    if (ev) {
        ev->a = mid;
        ev->b = tsk->pid;
        ev->c = cpu_id();
        ev->d = match ? 1 : 0;
        ev->u0 = mig_task_root_pa(tsk);
        ev->u1 = mig_task_asid(tsk);
        ev->u2 = (uint64_t)(uintptr_t)frame;
    }
    __atomic_store_n(&g_move_resumed[mid], true, __ATOMIC_RELEASE);
    if (mid == 1)
        mig_write_ctrl(MIG_CTRL_M1_GO, 1);
    else if (mid == 2)
        mig_write_ctrl(MIG_CTRL_M2_GO, 1);
    else if (mid == 3)
        mig_write_ctrl(MIG_CTRL_M3_GO, 1);
}

void aarch64_user_mig_idle_pre_wfi(uint32_t cpu, uint32_t rq_nr)
{
    if (!g_mig_active || cpu != 1)
        return;
    if (__atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE) != 1)
        return;
    if (!__atomic_load_n(&g_move_window_open[1], __ATOMIC_ACQUIRE) ||
        g_move1_wfi_ready)
        return;

    uint64_t deadline = mig_cntpct() + 5ULL * mig_cntfrq();
    while (!__atomic_load_n(&g_move_tail_seen[1], __ATOMIC_ACQUIRE) &&
           mig_cntpct() < deadline) {
        __asm__ volatile("yield" ::: "memory");
    }
    if (aarch64_sched_tick_mig_cadence(1) != 0 ||
        aarch64_sched_tick_mig_pending(1)) {
        return;
    }
    mig_reset_window_counters(1);
    __atomic_store_n(&g_move1_wfi_ready, true, __ATOMIC_RELEASE);
    struct mig_event *iz = mig_reserve(ME_IDLE_ZERO, NULL);
    if (iz) {
        iz->a = cpu;
        iz->b = rq_nr;
        iz->c = 0;
        iz->d = 0;
    }
    struct mig_event *wf = mig_reserve(ME_WFI_ENTER, NULL);
    if (wf)
        wf->a = cpu;
}

void aarch64_user_mig_note_irq(uint32_t intid)
{
    uint32_t cpu = cpu_id();
    if (!g_mig_active || cpu >= NR_CPUS)
        return;
    __atomic_store_n(&g_last_irq_intid[cpu], intid, __ATOMIC_RELAXED);
    if (intid < 16U)
        __atomic_fetch_add(&g_sgi_cnt[cpu], 1U, __ATOMIC_RELAXED);
    else if (intid != 30U)
        __atomic_fetch_add(&g_other_irq_cnt[cpu], 1U, __ATOMIC_RELAXED);
}

void aarch64_user_mig_note_ap_work(uint32_t cpu, uint32_t slot_state)
{
    if (!g_mig_active || cpu >= NR_CPUS)
        return;
    if (slot_state != 0)
        __atomic_fetch_add(&g_ap_work_cnt[cpu], 1U, __ATOMIC_RELAXED);
}

void aarch64_user_mig_note_sched_entry(uint32_t cpu, bool is_idle,
                                       uint32_t need_resched)
{
    if (!g_mig_active || cpu >= NR_CPUS)
        return;
    if (cpu == 0 && is_idle && need_resched)
        __atomic_fetch_add(&g_bsp_tick_req_seen, 1U, __ATOMIC_RELEASE);
    int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
    if (!is_idle && arch_current_task() != g_mig_worker_task &&
        ((mid == 1 && cpu == 1) || ((mid == 2 || mid == 3) && cpu == 0)))
        __atomic_fetch_add(&g_direct_sched_cnt[cpu], 1U, __ATOMIC_RELAXED);
}

void aarch64_user_mig_idle_tick(uint32_t cpu, uint32_t intid)
{
    if (!g_mig_active || cpu >= NR_CPUS)
        return;
    uint32_t real_intid = __atomic_load_n(&g_last_irq_intid[cpu], __ATOMIC_RELAXED);
    if (real_intid == 0)
        real_intid = intid;
    int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
    if (mid == 1 && cpu == 1 && g_move1_wfi_ready && !g_move1_took &&
        g_move1_tick_idx < 8) {
        int idx = ++g_move1_tick_idx;
        struct mig_event *ev = mig_reserve(ME_IDLE_TICK, NULL);
        if (ev) {
            ev->a = cpu;
            ev->b = real_intid;
            ev->c = idx;
        }
        return;
    }

    if (cpu == 0) {
        static uint32_t bsp_div;
        if ((++bsp_div & 3U) == 0 &&
            !((mid == 2 || mid == 3) && g_move_tail_seen[mid] && !g_move_committed[mid]))
            wait_queue_wake_one(&g_worker_wq);
    }
}

void aarch64_user_mig_idle_after_check(uint32_t cpu, bool took_balance,
                                       bool entered_wfi)
{
    if (!g_mig_active || cpu != 1 || !g_move1_wfi_ready || g_move1_took)
        return;
    uint32_t sgi = __atomic_load_n(&g_sgi_cnt[1], __ATOMIC_RELAXED);
    uint32_t oirq = __atomic_load_n(&g_other_irq_cnt[1], __ATOMIC_RELAXED);
    uint32_t apw = __atomic_load_n(&g_ap_work_cnt[1], __ATOMIC_RELAXED);
    uint32_t dsch = __atomic_load_n(&g_direct_sched_cnt[1], __ATOMIC_RELAXED);
    if (took_balance) {
        g_move1_took = true;
        struct mig_event *ev = mig_reserve(ME_IDLE_TAKE, NULL);
        if (ev) {
            ev->a = cpu;
            ev->u0 = sgi;
            ev->u1 = oirq;
            ev->u2 = apw;
            ev->u3 = dsch;
        }
    } else if (entered_wfi && g_move1_tick_idx >= 1 && g_move1_tick_idx <= 7) {
        struct mig_event *ev = mig_reserve(ME_IDLE_CHECK, NULL);
        if (ev) {
            ev->a = cpu;
            ev->b = g_move1_tick_idx;
            ev->c = 1;
            ev->u0 = sgi;
            ev->u1 = oirq;
            ev->u2 = apw;
            ev->u3 = dsch;
        }
    }
}

static const char *mig_vm_op_name(uint64_t nr, bool is_fault,
                                  enum mig_ev_kind *leaf_kind_out)
{
    if (is_fault) {
        if (leaf_kind_out)
            *leaf_kind_out = ME_EDIT_LEAF;
        return "fault";
    }
    if (nr == SYS_munmap) {
        if (leaf_kind_out)
            *leaf_kind_out = ME_EDIT_LEAF;
        return "munmap";
    }
    if (nr == SYS_mmap) {
        if (leaf_kind_out)
            *leaf_kind_out = ME_EDIT_VMA;
        return "mmap";
    }
    if (nr == SYS_mprotect) {
        if (leaf_kind_out)
            *leaf_kind_out = ME_EDIT_LEAF;
        return "mprotect";
    }
    return NULL;
}

void aarch64_user_mig_note_vm_begin(const struct task_struct *tsk)
{
    if (!g_mig_active || !tsk || (int)tsk->pid != g_main_pid || !g_vm_window_active)
        return;

    uint32_t tx = ++g_vm_tx_counter;
    g_active_vm_tx = tx;
    g_active_vm_tlbi_done = false;
    g_active_vm_eb = NULL;
    g_active_vm_td = NULL;

    struct mig_event *eb = mig_reserve(ME_EDIT_BEGIN, NULL);
    if (eb) {
        eb->a = tsk->pid;
        eb->b = cpu_id();
        eb->c = tx;
        eb->u0 = mig_task_root_pa(tsk);
        eb->u1 = mig_task_asid(tsk);
        __builtin_memcpy(eb->op, "vm", 3);
        g_active_vm_eb = eb;
    }
}

void aarch64_user_mig_note_va_tlbi(uint64_t va)
{
    if (!g_mig_active || !g_vm_window_active || va != MIG_DATA_VA)
        return;
    uint32_t tx = g_active_vm_tx;
    if (!tx || g_active_vm_tlbi_done)
        return;
    g_active_vm_tlbi_done = true;

    task_t *tsk = arch_current_task();
    struct mig_event *td = mig_reserve(ME_TLBI_DONE, NULL);
    if (td) {
        td->a = tsk ? tsk->pid : g_main_pid;
        td->b = cpu_id();
        td->c = tx;
        td->u0 = mig_task_root_pa(tsk);
        td->u1 = mig_task_asid(tsk);
        __builtin_memcpy(td->op, "vm", 3);
        g_active_vm_td = td;
    }
}

void aarch64_user_mig_note_vm_edit(const struct task_struct *tsk,
                                   uint64_t nr, uint64_t va, bool is_fault,
                                   uint64_t old_pte, uint64_t new_pte,
                                   bool leaf_absent)
{
    if (!g_mig_active || !tsk || (int)tsk->pid != g_main_pid ||
        !g_vm_window_active || va != MIG_DATA_VA)
        return;

    enum mig_ev_kind leaf_kind = ME_EDIT_LEAF;
    const char *op = mig_vm_op_name(nr, is_fault, &leaf_kind);
    uint32_t tx = g_active_vm_tx;
    if (!op || !tx)
        return;

    if (g_active_vm_eb)
        __builtin_memcpy(g_active_vm_eb->op, op, sizeof(g_active_vm_eb->op));
    if (g_active_vm_td)
        __builtin_memcpy(g_active_vm_td->op, op, sizeof(g_active_vm_td->op));

    uint64_t root = mig_task_root_pa(tsk);
    uint8_t asid = mig_task_asid(tsk);

    if (!g_active_vm_tlbi_done && (nr == SYS_mmap || is_fault)) {
        g_active_vm_tlbi_done = true;
        struct mig_event *td = mig_reserve(ME_TLBI_DONE, NULL);
        if (td) {
            td->a = tsk->pid;
            td->b = cpu_id();
            td->c = tx;
            td->u0 = root;
            td->u1 = asid;
            __builtin_memcpy(td->op, op, sizeof(td->op));
        }
    }

    struct mig_event *el = mig_reserve(leaf_kind, NULL);
    if (el) {
        el->a = tsk->pid;
        el->b = cpu_id();
        el->c = tx;
        el->d = leaf_absent ? 1 : 0;
        el->u0 = root;
        el->u1 = asid;
        el->u2 = old_pte;
        el->u3 = new_pte;
        __builtin_memcpy(el->op, op, sizeof(el->op));
    }
    g_active_vm_tx = 0;
    g_active_vm_eb = NULL;
    g_active_vm_td = NULL;
}

void aarch64_user_mig_note_tlbi(uint8_t asid, bool is_full_flush)
{
    if (!g_mig_active || !g_vm_window_active)
        return;
    if (is_full_flush) {
        struct mig_event *ev = mig_reserve(ME_FULL_FLUSH, NULL);
        if (ev) {
            ev->a = g_main_pid;
            ev->b = cpu_id();
            ev->u1 = asid;
        }
    } else {
        uint32_t tx = g_active_vm_tx;
        if (tx && !g_active_vm_tlbi_done) {
            g_active_vm_tlbi_done = true;
            task_t *tsk = arch_current_task();
            struct mig_event *td = mig_reserve(ME_TLBI_DONE, NULL);
            if (td) {
                td->a = tsk ? tsk->pid : g_main_pid;
                td->b = cpu_id();
                td->c = tx;
                td->u0 = mig_task_root_pa(tsk);
                td->u1 = asid;
                __builtin_memcpy(td->op, "vm", 3);
                g_active_vm_td = td;
            }
        }
        struct mig_event *ev = mig_reserve(ME_TLBI_TAG, NULL);
        if (ev) {
            ev->a = g_main_pid;
            ev->b = cpu_id();
            ev->c = tx;
            ev->u1 = asid;
        }
    }
}

void aarch64_user_mig_note_el0_svc(const struct task_struct *tsk,
                                   const struct pt_regs *frame)
{
    if (!g_mig_active || !tsk || !frame)
        return;
    int pid = (int)tsk->pid;

    uint64_t mf = spin_lock_irqsave(&g_mig_lock);
    if (g_main_pid < 0 && g_parent_pid > 0 && pid != g_parent_pid &&
        tsk->parent && (int)tsk->parent->pid == g_parent_pid) {
        g_main_pid = pid;
        struct mig_event *fev = mig_reserve(ME_FORK, NULL);
        if (fev) {
            fev->a = tsk->pid;
            fev->b = tsk->cpu;
            fev->u0 = tsk->allowed_cpus;
        }
    } else if (g_main_pid > 0 && g_child2_pid < 0 && pid != g_parent_pid &&
               pid != g_main_pid && tsk->parent &&
               (int)tsk->parent->pid == g_parent_pid) {
        g_child2_pid = pid;
        struct mig_event *fev = mig_reserve(ME_FORK, NULL);
        if (fev) {
            fev->a = tsk->pid;
            fev->b = tsk->cpu;
            fev->u0 = tsk->allowed_cpus;
        }
    }
    spin_unlock_irqrestore(&g_mig_lock, mf);

    if (pid == g_parent_pid && !g_parent_checked && g_ctrl_pa) {
        g_parent_checked = true;
        uint64_t pa = 0;
        uint64_t val = mig_read_user_u64(tsk, MIG_DATA_VA, &pa);
        uint64_t cow_val = mig_read_user_u64(tsk, MIG_COW_VA, NULL);
        struct mig_event *ev = mig_reserve(ME_PARENT_CHECK, NULL);
        if (ev) {
            ev->a = pid;
            ev->u0 = MIG_DATA_VA;
            ev->u1 = pa;
            ev->u2 = val;
            ev->u3 = cow_val;
        }
        return;
    }

    if (pid == g_main_pid) {
        if (!g_ctrl_pa && tsk->mm && tsk->mm->pgdir) {
            uint64_t cpa = 0;
            uint32_t cvm = 0;
            if (arch_vmm_query_4k((uint64_t *)Phy_To_Virt((uint64_t)(uintptr_t)tsk->mm->pgdir),
                                  MIG_CTRL_VA, &cpa, &cvm) == 0)
                __atomic_store_n(&g_ctrl_pa, cpa, __ATOMIC_RELEASE);
        }
        if (!g_cow_checked) {
            g_cow_checked = true;
            struct mig_event *cev = mig_reserve(ME_COW_CHECK, NULL);
            if (cev) {
                cev->a = pid;
                cev->u0 = MIG_COW_VA;
                cev->u2 = frame->x11;
            }
        }
        if (!g_old_load_checked) {
            g_old_load_checked = true;
            uint64_t pa = 0;
            (void)mig_read_user_u64(tsk, MIG_DATA_VA, &pa);
            struct mig_event *oev = mig_reserve(ME_OLD_LOAD, NULL);
            if (oev) {
                oev->a = pid;
                oev->b = cpu_id();
                oev->u0 = mig_task_root_pa(tsk);
                oev->u1 = mig_task_asid(tsk);
                oev->u2 = MIG_DATA_VA;
                oev->u3 = pa;
                oev->c = (int64_t)frame->x12;
            }
            g_vm_window_active = true;
            return;
        }
        if (g_move_resumed[1] && !g_move_el0_seen[1]) {
            g_move_el0_seen[1] = true;
            uint64_t t0 = 0, t1 = 0;
            aarch64_user_vm_save_tls(&t0, &t1);
            struct mig_event *ev = mig_reserve(ME_EL0_AFTER, NULL);
            if (ev) {
                ev->a = 1;
                ev->b = pid;
                ev->c = cpu_id();
                ev->u0 = mig_task_root_pa(tsk);
                ev->u1 = mig_task_asid(tsk);
                ev->u2 = frame->x13;
                ev->u3 = t0;
            }
            return;
        }
        if (g_move_el0_seen[1] && !g_remap_checked) {
            g_remap_checked = true;
            uint64_t pa = 0;
            uint64_t val = mig_read_user_u64(tsk, MIG_DATA_VA, &pa);
            struct mig_event *ev = mig_reserve(ME_REMAP, NULL);
            if (ev) {
                ev->a = pid;
                ev->b = cpu_id();
                ev->u0 = mig_task_root_pa(tsk);
                ev->u1 = mig_task_asid(tsk);
                ev->u2 = MIG_DATA_VA;
                ev->u3 = pa;
                ev->c = (int64_t)val;
            }
            return;
        }
        if (g_move_resumed[2] && !g_move_el0_seen[2]) {
            g_move_el0_seen[2] = true;
            uint64_t t0 = 0, t1 = 0;
            aarch64_user_vm_save_tls(&t0, &t1);
            struct mig_event *ev = mig_reserve(ME_EL0_AFTER, NULL);
            if (ev) {
                ev->a = 2;
                ev->b = pid;
                ev->c = cpu_id();
                ev->u0 = mig_task_root_pa(tsk);
                ev->u1 = mig_task_asid(tsk);
                ev->u2 = frame->x13;
                ev->u3 = t0;
            }
            return;
        }
    }

    if (pid == g_child2_pid && g_exec_committed && !g_exec_el0_seen)
        aarch64_user_mig_note_exec_el0(tsk);

    if (pid == g_child2_pid && g_move_resumed[3] && !g_move_el0_seen[3]) {
        g_move_el0_seen[3] = true;
        uint64_t t0 = 0, t1 = 0;
        aarch64_user_vm_save_tls(&t0, &t1);
        struct mig_event *ev = mig_reserve(ME_EL0_AFTER, NULL);
        if (ev) {
            ev->a = 3;
            ev->b = pid;
            ev->c = cpu_id();
            ev->u0 = mig_task_root_pa(tsk);
            ev->u1 = mig_task_asid(tsk);
            ev->u2 = frame->x13;
            ev->u3 = t0;
        }
    }
}

void aarch64_user_mig_note_sync_fault(const struct task_struct *tsk,
                                      const struct pt_regs *frame,
                                      uint64_t esr, uint64_t far, int frc)
{
    if (!g_mig_active || !tsk || !frame || (int)tsk->pid != g_main_pid || frc == 0)
        return;
    uint64_t pa = 0;
    (void)mig_read_user_u64(tsk, far, &pa);
    struct mig_event *nev = mig_reserve(ME_NEW_LOAD, NULL);
    if (nev) {
        nev->a = tsk->pid;
        nev->b = cpu_id();
        nev->u0 = mig_task_root_pa(tsk);
        nev->u1 = mig_task_asid(tsk);
        nev->u2 = far;
        nev->u3 = pa;
        nev->c = (int64_t)frame->x10;
    }
    struct mig_event *pev = mig_reserve(ME_PERM_ABORT, NULL);
    if (pev) {
        pev->a = tsk->pid;
        pev->b = cpu_id();
        pev->u0 = mig_task_root_pa(tsk);
        pev->u1 = mig_task_asid(tsk);
        pev->u2 = far;
        pev->u3 = pa;
        pev->c = (int64_t)esr;
        pev->d = (int64_t)frame->x10;
    }
    g_vm_window_active = false;
}

void aarch64_user_mig_note_copy(const struct task_struct *tsk, bool write,
                                uint64_t user_addr, int64_t ret)
{
    if (!g_mig_active || !tsk || (int)tsk->pid != g_child2_pid || write)
        return;
    if (user_addr != 0x40002040ULL && user_addr != 0x40003000ULL)
        return;
    struct mig_event *ev = mig_reserve(ret >= 0 ? ME_COPY_OK : ME_COPY_DENIED, NULL);
    if (ev) {
        ev->a = tsk->pid;
        ev->b = cpu_id();
        ev->c = ret;
    }
}

void aarch64_user_mig_note_wait_status(int req_pid, int child_pid,
                                       int exit_code, int copy_rc)
{
    if (!g_mig_active)
        return;
    if (child_pid != g_main_pid && child_pid != g_child2_pid)
        return;
    struct mig_event *ev = mig_reserve(ME_WAIT_STATUS, NULL);
    if (ev) {
        ev->a = req_pid > 0 ? req_pid : child_pid;
        ev->b = child_pid;
        ev->c = (copy_rc < 0) ? copy_rc : exit_code;
    }
}

void aarch64_user_mig_note_exec_commit(const struct task_struct *tsk,
                                       uint64_t old_root, uint64_t new_root,
                                       uint64_t old_mask, uint64_t new_mask)
{
    if (!g_mig_active || !tsk || (int)tsk->pid != g_child2_pid)
        return;
    g_exec_committed = true;
    struct mig_event *ev = mig_reserve(ME_EXEC_COMMIT, NULL);
    if (ev) {
        ev->a = tsk->pid;
        ev->b = cpu_id();
        ev->u0 = old_root;
        ev->u1 = new_root;
        ev->u2 = old_mask;
        ev->u3 = new_mask;
    }
}

void aarch64_user_mig_note_exec_el0(const struct task_struct *tsk)
{
    if (!g_mig_active || !tsk || (int)tsk->pid != g_child2_pid ||
        !g_exec_committed || g_exec_el0_seen)
        return;
    g_exec_el0_seen = true;
    struct mig_event *ev = mig_reserve(ME_EXEC_EL0, NULL);
    if (ev) {
        ev->a = tsk->pid;
        ev->b = cpu_id();
        ev->u0 = mig_task_root_pa(tsk);
        ev->u2 = tsk->allowed_cpus;
    }
}

void aarch64_user_mig_note_bsp_sched(uint32_t cpu) { (void)cpu; }

void aarch64_user_runtime_test_timer_eoi(uint32_t intid, struct pt_regs *regs)
{
    (void)intid;
    (void)regs;
    if (!g_mig_active)
        return;
    task_t *cur = arch_current_task();
    if (!cur)
        return;
    uint32_t cpu = cpu_id();
    int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
    if (mid == 1 && cpu == 0 && (int)cur->pid == g_main_pid &&
        __atomic_load_n(&g_move_window_open[1], __ATOMIC_ACQUIRE) &&
        cur->vruntime >= cur->deadline) {
        wait_queue_wake_one(&g_helper_wq[0]);
    } else if (mid == 2 && cpu == 1 && (int)cur->pid == g_main_pid &&
               __atomic_load_n(&g_move_window_open[2], __ATOMIC_ACQUIRE) &&
               cur->vruntime >= cur->deadline) {
        wait_queue_wake_one(&g_helper_wq[1]);
    } else if (mid == 3 && cpu == 1 && (int)cur->pid == g_child2_pid &&
               __atomic_load_n(&g_move_window_open[3], __ATOMIC_ACQUIRE) &&
               cur->vruntime >= cur->deadline) {
        wait_queue_wake_one(&g_helper_wq[1]);
    }
}
void aarch64_user_runtime_test_irq_sched_before(struct task_struct *tsk,
    const struct pt_regs *regs, uint64_t *switches_out, uint64_t *elr_out)
{ (void)tsk; (void)regs; if (switches_out) *switches_out = 0; if (elr_out) *elr_out = 0; }
void aarch64_user_runtime_test_irq_sched_after(struct task_struct *tsk,
    const struct pt_regs *regs, uint64_t switches_before, uint64_t elr_before)
{ (void)tsk; (void)regs; (void)switches_before; (void)elr_before; }
void aarch64_user_runtime_test_vm_edit_pause(struct mm_struct *mm) { (void)mm; }
void aarch64_user_runtime_test_vm_edit_after(struct mm_struct *mm) { (void)mm; }
void aarch64_user_runtime_test_sigreturn(struct task_struct *tsk) { (void)tsk; }
void aarch64_user_runtime_test_note_switch(const struct task_struct *prev,
    const struct task_struct *next) { (void)prev; (void)next; }
void aarch64_user_runtime_test_note_ttbr_switch(uint64_t old_ttbr0,
    uint64_t new_ttbr0, bool full_flush)
{
    (void)old_ttbr0;
    (void)new_ttbr0;
    if (!g_mig_active || !g_vm_window_active || !full_flush)
        return;
    struct mig_event *ev = mig_reserve(ME_FULL_FLUSH, NULL);
    if (ev) {
        ev->a = g_main_pid;
        ev->b = cpu_id();
        ev->u1 = (uint8_t)(new_ttbr0 >> 48);
    }
}
void aarch64_user_runtime_test_note_asid_tlbi(uint8_t asid) { (void)asid; }
void aarch64_user_runtime_test_note_asid_release(uint8_t asid) { (void)asid; }
void aarch64_user_runtime_test_cow_barrier(uint64_t va, uint64_t old_pa) { (void)va; (void)old_pa; }
void aarch64_user_runtime_test_cow_last_free(uint64_t old_pa) { (void)old_pa; }
void aarch64_user_runtime_test_before_exit(struct task_struct *tsk) { (void)tsk; }
void aarch64_user_runtime_test_explicit_pin_el0(const struct task_struct *tsk) { (void)tsk; }

static bool mig_wait_helper_pred(void *arg)
{
    uint32_t cpu = (uint32_t)(uintptr_t)arg;
    if (__atomic_load_n(&g_helper_stop, __ATOMIC_ACQUIRE))
        return true;
    int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
    if (mid == 1 && cpu == 0)
        return __atomic_load_n(&g_move_window_open[1], __ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&g_move_committed[1], __ATOMIC_ACQUIRE);
    if ((mid == 2 || mid == 3) && cpu == 1)
        return __atomic_load_n(&g_move_window_open[mid], __ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&g_move_committed[mid], __ATOMIC_ACQUIRE);
    return false;
}

static uint64_t mig_helper_fn(uint64_t arg)
{
    uint32_t cpu = (uint32_t)arg;
    while (!__atomic_load_n(&g_helper_stop, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_helper_waiting[cpu], true, __ATOMIC_RELEASE);
        while (!mig_wait_helper_pred((void *)(uintptr_t)cpu))
            wait_queue_sleep(&g_helper_wq[cpu]);
        __atomic_store_n(&g_helper_waiting[cpu], false, __ATOMIC_RELEASE);
        if (__atomic_load_n(&g_helper_stop, __ATOMIC_ACQUIRE))
            break;

        int mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
        if (mid >= 1 && mid <= 3) {
            uint64_t dl = mig_cntpct() + 5ULL * mig_cntfrq();
            while (!__atomic_load_n(&g_move_committed[mid], __ATOMIC_ACQUIRE) &&
                   mig_cntpct() < dl &&
                   !__atomic_load_n(&g_helper_stop, __ATOMIC_ACQUIRE)) {
                __asm__ volatile("yield" ::: "memory");
            }
        }
    }
    __atomic_store_n(&g_helper_done[cpu], true, __ATOMIC_RELEASE);
    return 0;
}

static int mig_write_vfs_file(const char *path, const void *data, size_t len)
{
    vfs_node_t *node = NULL;
    int rc = vfs_create_file(path, NULL, &node);
    if (rc < 0 || !node)
        return rc < 0 ? rc : -EIO;
    if (len > 0) {
        ssize_t wr = vfs_write(node, 0, len, (void *)(uintptr_t)data);
        if (wr != (ssize_t)len) {
            vfs_node_put(node);
            return -EIO;
        }
    }
    vfs_node_put(node);
    return 0;
}

static void mig_dump_events(void)
{
    uint32_t total = __atomic_load_n(&g_mig_seq, __ATOMIC_ACQUIRE);
    if (total > MIG_MAX_EVENTS)
        total = MIG_MAX_EVENTS;
    for (uint32_t s = 1; s <= total; s++) {
        const struct mig_event *ev = &g_events[s - 1U];
        switch (ev->kind) {
        case ME_BASELINE:
            log_info("[user-runtime] migration-event seq=%u kind=baseline roots=%d pages=%d asids=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c);
            break;
        case ME_FINAL:
            log_info("[user-runtime] migration-event seq=%u kind=final roots=%d pages=%d asids=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c);
            break;
        case ME_SPAWN:
            log_info("[user-runtime] migration-event seq=%u kind=spawn pid=%d cpu=%d mask=%lu\n",
                     s, (int)ev->a, (int)ev->b, (unsigned long long)ev->u0);
            break;
        case ME_FORK:
            log_info("[user-runtime] migration-event seq=%u kind=fork pid=%d cpu=%d mask=%lu\n",
                     s, (int)ev->a, (int)ev->b, (unsigned long long)ev->u0);
            break;
        case ME_PARENT_CHECK:
            log_info("[user-runtime] migration-event seq=%u kind=parent_check pid=%d va=%lx pa=%lx data=%lx cow_val=%lx\n",
                     s, (int)ev->a, (unsigned long long)ev->u0,
                     (unsigned long long)ev->u1, (unsigned long long)ev->u2,
                     (unsigned long long)ev->u3);
            break;
        case ME_COW_CHECK:
            log_info("[user-runtime] migration-event seq=%u kind=cow_check pid=%d va=%lx cow_val=%lx\n",
                     s, (int)ev->a, (unsigned long long)ev->u0,
                     (unsigned long long)ev->u2);
            break;
        case ME_OLD_LOAD:
            log_info("[user-runtime] migration-event seq=%u kind=old_load pid=%d cpu=%d root=%lx asid=%lu va=%lx pa=%lx data=%lx\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3,
                     (unsigned long long)ev->c);
            break;
        case ME_REMAP:
            log_info("[user-runtime] migration-event seq=%u kind=remap pid=%d cpu=%d root=%lx asid=%lu va=%lx pa=%lx data=%lx\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3,
                     (unsigned long long)ev->c);
            break;
        case ME_NEW_LOAD:
            log_info("[user-runtime] migration-event seq=%u kind=new_load pid=%d cpu=%d root=%lx asid=%lu va=%lx pa=%lx data=%lx\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3,
                     (unsigned long long)ev->c);
            break;
        case ME_PERM_ABORT: {
            uint64_t esr = (uint64_t)ev->c;
            uint64_t ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
            uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;
            uint64_t wnr = (esr & AARCH64_ESR_ISS_WNR) ? 1ULL : 0ULL;
            log_info("[user-runtime] migration-event seq=%u kind=permission_abort pid=%d cpu=%d root=%lx asid=%lu va=%lx pa=%lx data=%lx ec=%lx fsc=%lx far=%lx wnr=%lu\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3,
                     (unsigned long long)ev->d, (unsigned long long)ec,
                     (unsigned long long)fsc, (unsigned long long)ev->u2,
                     (unsigned long long)wnr);
            break;
        }
        case ME_EDIT_BEGIN:
            log_info("[user-runtime] migration-event seq=%u kind=edit_begin pid=%d cpu=%d root=%lx asid=%lu tx=%d op=%s\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->c, ev->op);
            break;
        case ME_TLBI_DONE:
            log_info("[user-runtime] migration-event seq=%u kind=tlbi_done pid=%d cpu=%d root=%lx asid=%lu tx=%d op=%s\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->c, ev->op);
            break;
        case ME_EDIT_LEAF:
            log_info("[user-runtime] migration-event seq=%u kind=edit_leaf pid=%d cpu=%d root=%lx asid=%lu tx=%d op=%s old_pte=%lx new_pte=%lx\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->c, ev->op,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3);
            break;
        case ME_EDIT_VMA:
            log_info("[user-runtime] migration-event seq=%u kind=edit_vma pid=%d cpu=%d root=%lx asid=%lu tx=%d op=%s leaf_absent=%d\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->c, ev->op, (int)ev->d);
            break;
        case ME_TLBI_TAG:
            log_info("[user-runtime] migration-event seq=%u kind=tlbi_tag pid=%d cpu=%d tx=%d asid=%lu\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u1);
            break;
        case ME_FULL_FLUSH:
            log_info("[user-runtime] migration-event seq=%u kind=full_flush pid=%d cpu=%d asid=%lu\n",
                     s, (int)ev->a, (int)ev->b, (unsigned long long)ev->u1);
            break;
        case ME_IRQ_SAVE:
            log_info("[user-runtime] migration-event seq=%u kind=irq_save move_id=%d pid=%d cpu=%d root=%lx asid=%lu frame=%lx\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2);
            break;
        case ME_HW_DETACH:
            log_info("[user-runtime] migration-event seq=%u kind=hw_detach move_id=%d pid=%d cpu=%d root=%lx asid=%lu ttbr0=%lx\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2);
            break;
        case ME_ROOT_UNPUBLISH:
            log_info("[user-runtime] migration-event seq=%u kind=root_unpublish move_id=%d pid=%d cpu=%d root=%lx asid=%lu\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1);
            break;
        case ME_TAIL:
            log_info("[user-runtime] migration-event seq=%u kind=tail move_id=%d pid=%d cpu=%d root=%lx asid=%lu on_cpu=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->d);
            break;
        case ME_COMMIT:
            log_info("[user-runtime] migration-event seq=%u kind=commit move_id=%d pid=%d cpu=%d root=%lx asid=%lu src=%d dst=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->d, (int)ev->e);
            break;
        case ME_PICK:
            log_info("[user-runtime] migration-event seq=%u kind=pick move_id=%d pid=%d cpu=%d root=%lx asid=%lu on_cpu=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (int)ev->d);
            break;
        case ME_HW_INSTALL:
            log_info("[user-runtime] migration-event seq=%u kind=hw_install move_id=%d pid=%d cpu=%d root=%lx asid=%lu\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1);
            break;
        case ME_IRQ_RESUME:
            log_info("[user-runtime] migration-event seq=%u kind=irq_resume move_id=%d pid=%d cpu=%d root=%lx asid=%lu frame=%lx match=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (int)ev->d);
            break;
        case ME_EL0_AFTER:
            log_info("[user-runtime] migration-event seq=%u kind=el0_after move_id=%d pid=%d cpu=%d root=%lx asid=%lu x13=%lx tpidr=%lx\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3);
            break;
        case ME_IDLE_ZERO:
            log_info("[user-runtime] migration-event seq=%u kind=idle_zero cpu=%d rq_nr=%d pending=%d cadence=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c, (int)ev->d);
            break;
        case ME_WFI_ENTER:
            log_info("[user-runtime] migration-event seq=%u kind=wfi_enter cpu=%d\n",
                     s, (int)ev->a);
            break;
        case ME_IDLE_TICK:
            log_info("[user-runtime] migration-event seq=%u kind=idle_tick cpu=%d intid=%d idx=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c);
            break;
        case ME_IDLE_CHECK:
            log_info("[user-runtime] migration-event seq=%u kind=idle_check cpu=%d idx=%d wfi=%d sgi=%lu other_irq=%lu ap_work=%lu direct_sched=%lu\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3);
            break;
        case ME_IDLE_TAKE:
            log_info("[user-runtime] migration-event seq=%u kind=idle_take cpu=%d sgi=%lu other_irq=%lu ap_work=%lu direct_sched=%lu\n",
                     s, (int)ev->a,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3);
            break;
        case ME_BSP_TICK_SCHED:
            log_info("[user-runtime] migration-event seq=%u kind=bsp_tick_schedule move_id=%d cpu=%d bsp_req=%d sgi=%lu ap_work=%lu direct_sched=%lu\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2);
            break;
        case ME_COPY_OK:
            log_info("[user-runtime] migration-event seq=%u kind=copy_ok pid=%d cpu=%d ret=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c);
            break;
        case ME_COPY_DENIED:
            log_info("[user-runtime] migration-event seq=%u kind=copy_denied pid=%d cpu=%d ret=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c);
            break;
        case ME_EXEC_COMMIT:
            log_info("[user-runtime] migration-event seq=%u kind=exec_commit pid=%d cpu=%d old_root=%lx new_root=%lx old_mask=%lu new_mask=%lu\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u1,
                     (unsigned long long)ev->u2, (unsigned long long)ev->u3);
            break;
        case ME_EXEC_EL0:
            log_info("[user-runtime] migration-event seq=%u kind=exec_el0 pid=%d cpu=%d root=%lx mask=%lu\n",
                     s, (int)ev->a, (int)ev->b,
                     (unsigned long long)ev->u0, (unsigned long long)ev->u2);
            break;
        case ME_WAIT_STATUS:
            log_info("[user-runtime] migration-event seq=%u kind=wait_status child_pid=%d returned_pid=%d status=%d\n",
                     s, (int)ev->a, (int)ev->b, (int)ev->c);
            break;
        default:
            break;
        }
    }
}

static uint64_t mig_reap_pid_seen[8];
static uint64_t mig_reap_status_seen[8];
static volatile int mig_reap_idx;

static void mig_reap_observer(struct task_struct *t)
{
    if (!t || t->state != TASK_ZOMBIE)
        return;
    int pid = (int)t->pid;
    uint64_t status = (uint64_t)t->exit_code;
    int i = __atomic_load_n(&mig_reap_idx, __ATOMIC_RELAXED);
    if (i < 8) {
        mig_reap_pid_seen[i] = (uint64_t)pid;
        mig_reap_status_seen[i] = status;
        __atomic_store_n(&mig_reap_idx, i + 1, __ATOMIC_RELEASE);
    }
    if (pid == g_parent_pid || pid == g_main_pid || pid == g_child2_pid)
        wait_queue_wake_one(&g_worker_wq);
}

static uint64_t mig_worker(uint64_t arg)
{
    (void)arg;
    g_mig_worker_task = current;
    int rc = 1;
    uint64_t roots_before = aarch64_pt_published_root_count();
    uint64_t pages_before = pmm_probe_4k_allocated_count();
    uint64_t asids_before = aarch64_asid_test_in_use_count();

    if (mig_write_vfs_file(MIG_PATH_MAIN, _binary_aarch64_user_migration_elf_start,
                           (size_t)(_binary_aarch64_user_migration_elf_end -
                                    _binary_aarch64_user_migration_elf_start)) != 0 ||
        mig_write_vfs_file(MIG_PATH_EXEC, _binary_aarch64_user_migration_exec_elf_start,
                           (size_t)(_binary_aarch64_user_migration_exec_elf_end -
                                    _binary_aarch64_user_migration_exec_elf_start)) != 0 ||
        mig_write_vfs_file(MIG_PATH_COPY, NULL, 0) != 0) {
        log_err("[user-runtime] FAIL case=migration reason=vfs_stage\n");
        goto out;
    }

    wait_queue_init(&g_helper_wq[0]);
    wait_queue_init(&g_helper_wq[1]);
    wait_queue_init(&g_worker_wq);
    kernel_thread_on_cpu(mig_helper_fn, 0, 0);
    kernel_thread_on_cpu(mig_helper_fn, 1, 1);

    uint64_t dl = mig_cntpct() + 5ULL * mig_cntfrq();
    while ((!__atomic_load_n(&g_helper_waiting[0], __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&g_helper_waiting[1], __ATOMIC_ACQUIRE)) &&
           mig_cntpct() < dl) {
        kthread_yield();
    }

    g_mig_active = true;
    aarch64_sched_set_reap_observer(mig_reap_observer);

    struct mig_event *bev = mig_reserve(ME_BASELINE, NULL);
    if (bev) {
        bev->a = (int64_t)roots_before;
        bev->b = (int64_t)pages_before;
        bev->c = (int64_t)asids_before;
    }

    files_t *spawn_files = NULL;
    bool restore_idle_files = false;
    if (!percpu_data[0].idle->files) {
        spawn_files = files_alloc();
        if (!spawn_files) {
            log_err("[user-runtime] FAIL case=migration reason=files_alloc\n");
            goto out_stop;
        }
        percpu_data[0].idle->files = spawn_files;
        restore_idle_files = true;
    }
    const char *argv[] = { MIG_PATH_MAIN, NULL };
    g_parent_pid = (int)spawn_user_task(MIG_PATH_MAIN, argv);
    if (restore_idle_files) {
        percpu_data[0].idle->files = NULL;
        files_unpin(spawn_files);
    }
    if (g_parent_pid < 0) {
        log_err("[user-runtime] FAIL case=migration reason=spawn_failed rc=%d\n",
                g_parent_pid);
        goto out_stop;
    }
    uint32_t spawn_cpu = 0;
    uint64_t spawn_mask = 0;
    {
        uint64_t tf = spin_lock_irqsave(&task_list_lock);
        for (list_t *p = init_task_union.task.list.next;
             p != &init_task_union.task.list; p = p->next) {
            task_t *t = container_of(p, task_t, list);
            if ((int)t->pid == g_parent_pid) {
                spawn_cpu = t->cpu;
                spawn_mask = t->allowed_cpus;
                break;
            }
        }
        spin_unlock_irqrestore(&task_list_lock, tf);
    }
    struct mig_event *sev = mig_reserve(ME_SPAWN, NULL);
    if (sev) {
        sev->a = g_parent_pid;
        sev->b = spawn_cpu;
        sev->u0 = spawn_mask;
    }

    /* Wait for parent + child1 + child2 to finish all stages. */
    dl = mig_cntpct() + 30ULL * mig_cntfrq();
    while (mig_cntpct() < dl) {
        if (g_main_pid < 0 || g_child2_pid < 0) {
            /* Discover forked children from task_list_lock. */
            uint64_t tf = spin_lock_irqsave(&task_list_lock);
            uint64_t mf = spin_lock_irqsave(&g_mig_lock);
            for (list_t *p = init_task_union.task.list.next;
                 p != &init_task_union.task.list; p = p->next) {
                task_t *t = container_of(p, task_t, list);
                if (t->parent && (int)t->parent->pid == g_parent_pid) {
                    if (g_main_pid < 0) {
                        g_main_pid = (int)t->pid;
                        struct mig_event *fev = mig_reserve(ME_FORK, NULL);
                        if (fev) {
                            fev->a = t->pid;
                            fev->b = t->cpu;
                            fev->u0 = t->allowed_cpus;
                        }
                    } else if ((int)t->pid != g_main_pid && g_child2_pid < 0) {
                        g_child2_pid = (int)t->pid;
                        struct mig_event *fev = mig_reserve(ME_FORK, NULL);
                        if (fev) {
                            fev->a = t->pid;
                            fev->b = t->cpu;
                            fev->u0 = t->allowed_cpus;
                        }
                    }
                }
            }
            spin_unlock_irqrestore(&g_mig_lock, mf);
            spin_unlock_irqrestore(&task_list_lock, tf);
        }

        if (g_main_pid > 0 && __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE) == 0 &&
            g_old_load_checked && mig_read_ctrl(MIG_CTRL_C1) == 1) {
            __atomic_store_n(&g_active_move_id, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_move_window_open[1], true, __ATOMIC_RELEASE);
            log_info("[user-runtime] mig-ctrl: opened move 1 window\n");
        } else if (__atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE) == 1 &&
                   g_remap_checked && mig_read_ctrl(MIG_CTRL_C1) == 3) {
            __atomic_store_n(&g_move_window_open[1], false, __ATOMIC_RELEASE);
            mig_reset_window_counters(0);
            __atomic_store_n(&g_active_move_id, 2, __ATOMIC_RELEASE);
            __atomic_store_n(&g_move_window_open[2], true, __ATOMIC_RELEASE);
        } else if (__atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE) == 2 &&
                   g_child2_pid > 0 && mig_read_ctrl(MIG_CTRL_C2) == 1) {
            __atomic_store_n(&g_move_window_open[2], false, __ATOMIC_RELEASE);
            mig_reset_window_counters(0);
            __atomic_store_n(&g_active_move_id, 3, __ATOMIC_RELEASE);
            __atomic_store_n(&g_move_window_open[3], true, __ATOMIC_RELEASE);
        }

        bool parent_reaped = false;
        int parent_status = -1;
        int reap_cnt = __atomic_load_n(&mig_reap_idx, __ATOMIC_ACQUIRE);
        for (int i = 0; i < reap_cnt && i < 8; i++) {
            if ((int)mig_reap_pid_seen[i] == g_parent_pid) {
                parent_reaped = true;
                parent_status = (int)mig_reap_status_seen[i];
            }
        }
        if (parent_reaped) {
            rc = (parent_status == 0 && !g_mig_overflow) ? 0 : 1;
            break;
        }
        int cur_mid = __atomic_load_n(&g_active_move_id, __ATOMIC_ACQUIRE);
        if ((cur_mid == 2 && !g_move_resumed[2]) ||
            (cur_mid == 3 && !g_move_resumed[3]))
            wait_queue_sleep(&g_worker_wq);
        else
            kthread_yield();
    }

out_stop:
    g_mig_active = false;
    aarch64_sched_set_reap_observer(NULL);
    __atomic_store_n(&g_helper_stop, true, __ATOMIC_RELEASE);
    wait_queue_wake_one(&g_helper_wq[0]);
    wait_queue_wake_one(&g_helper_wq[1]);
    dl = mig_cntpct() + 5ULL * mig_cntfrq();
    while ((!g_helper_done[0] || !g_helper_done[1]) && mig_cntpct() < dl)
        kthread_yield();

    vfs_unlink(MIG_PATH_MAIN, NULL);
    vfs_unlink(MIG_PATH_EXEC, NULL);
    vfs_unlink(MIG_PATH_COPY, NULL);

    uint64_t roots_after = aarch64_pt_published_root_count();
    uint64_t pages_after = pmm_probe_4k_allocated_count();
    uint64_t asids_after = aarch64_asid_test_in_use_count();
    struct mig_event *fev = mig_reserve(ME_FINAL, NULL);
    if (fev) {
        fev->a = (int64_t)roots_after;
        fev->b = (int64_t)pages_after;
        fev->c = (int64_t)asids_after;
    }

    mig_dump_events();

    if (rc == 0 && roots_after == roots_before &&
        pages_after == pages_before && asids_after == asids_before) {
        log_info("[user-runtime] PASS case=migration cpu_mask=3\n");
    } else {
        log_err("[user-runtime] FAIL case=migration rc=%d roots=%lu/%lu pages=%lu/%lu asids=%lu/%lu p=%d m=%d c2=%d r0=%lu/%lu r1=%lu/%lu r2=%lu/%lu\n",
                rc, (unsigned long)roots_after, (unsigned long)roots_before,
                (unsigned long)pages_after, (unsigned long)pages_before,
                (unsigned long)asids_after, (unsigned long)asids_before,
                g_parent_pid, g_main_pid, g_child2_pid,
                (unsigned long)mig_reap_pid_seen[0], (unsigned long)mig_reap_status_seen[0],
                (unsigned long)mig_reap_pid_seen[1], (unsigned long)mig_reap_status_seen[1],
                (unsigned long)mig_reap_pid_seen[2], (unsigned long)mig_reap_status_seen[2]);
        rc = 1;
    }
out:
    g_mig_rc = rc;
    __atomic_store_n(&g_mig_done, true, __ATOMIC_RELEASE);
    return (uint64_t)rc;
}

void test_aarch64_user_migration_run(void)
{
    g_mig_done = false;
    g_mig_rc = 1;
    kernel_thread_on_cpu(mig_worker, 0, 0);

    uint64_t dl = mig_cntpct() + 60ULL * mig_cntfrq();
    while (!__atomic_load_n(&g_mig_done, __ATOMIC_ACQUIRE) && mig_cntpct() < dl)
        arch_aarch64_idle_step();

    int rc = __atomic_load_n(&g_mig_done, __ATOMIC_ACQUIRE) ? g_mig_rc : 1;
    selftest_record_late("aarch64_user_runtime_migration", rc);
}
#endif /* AARCH64_USER_RUNTIME_CASE_MIGRATION */
