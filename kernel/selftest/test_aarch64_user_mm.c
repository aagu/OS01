#if AARCH64_EL0_TEST

/* kernel/selftest/test_aarch64_user_mm.c — aarch64 M4 acceptance (plan
 * Task 3): one mm-owned user root exercised through the M3 4 KiB
 * entries across three EL0 entries, the -EBUSY reject path from the
 * round-1 SVC callback, the thread-context rejection matrix, the
 * empty-L2-branch destroy scan and full PMM reclaim back to baseline.
 * Single SMP=1 boot; the worker is a CPU0 kthread driving
 * aarch64_el0_run_kthread() exactly like the M4.7 probe. */

#include <arch/aarch64/el0.h>
#include <arch/aarch64/user_mm.h>
#include <arch/aarch64/dtb.h>
#include <arch/mmu.h>
#include <arch/aarch64/regs.h>
#include <errno.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <stddef.h>

#define UM_CODE_VA   0x400000
#define UM_DATA_VA   (UM_CODE_VA + PAGE_4K_SIZE)
#define UM_STACK_VA  (UM_CODE_VA + 2 * PAGE_4K_SIZE)
#define UM_STACK_TOP (UM_CODE_VA + 3 * PAGE_4K_SIZE)
/* Fresh slot in the same L3 table as the code/data/stack branch — used
 * for the in-run -EBUSY map attempt and the bare-RW/unallocated-PA
 * rejects (the -EBUSY path never creates the leaf). */
#define UM_SVC_VA    (UM_CODE_VA + 4 * PAGE_4K_SIZE)
/* Independent L2 branch (L1[0].L2[256]): mapped then unmapped to leave
 * two empty tables in front of the branch that still has leaves. */
#define UM_L2X_VA    0x20000000
#define UM_KVA       0xffff800000100000ULL   /* kernel-half → -EINVAL */
#define UM_VALUE     0x5a
#define UM_STATUS    (UM_VALUE + 2)

/* Relocation-free probe: three entries sharing one page.  x0 carries
 * the data VA.  R1 reads, increments and stores the known value, then
 * reports x1 = VALUE+1 via svc #8.  R2 reads back (old value visible
 * in x1) then attempts the write the RO leaf must fault.  R3 loads the
 * unmapped VA and must take a translation fault. */
__asm__(
    ".pushsection .text, \"ax\"\n"
    ".balign 4\n"
    ".global umm_probe_start\n"
    "umm_probe_start:\n"
    "umm_probe_r1:\n"
    "    ldr x1, [x0]\n"
    "    add x1, x1, #1\n"
    "    str x1, [x0]\n"
    "    svc #8\n"
    "umm_probe_r2:\n"
    "    ldr x1, [x0]\n"
    "    str x1, [x0]\n"
    "    svc #9\n"
    "umm_probe_r3:\n"
    "    ldr x1, [x0]\n"
    "    svc #9\n"
    ".global umm_probe_end\n"
    "umm_probe_end:\n"
    ".popsection\n");
extern const char umm_probe_start[], umm_probe_end[];
extern const char umm_probe_r1[], umm_probe_r2[], umm_probe_r3[];

struct umm_run {
    int pid, rc;
    uint64_t value;
    mm_t *mm;
    uint64_t *root;             /* direct-map root pointer */
    uint64_t root_pa;
    uint64_t code_pa, data_pa, stack_pa;
    uint64_t svc_ttbr1;         /* TTBR1 snapshot at round start */

    unsigned phase;             /* 1..3 while a round is installed */
    bool r1, r2, r3;            /* per-round trap contracts */
    bool busy_map, busy_update, busy_unmap;
    bool ctx_ok;                /* SVC-callback context checks */
    bool matrix_ok;             /* thread-context rejection matrix */
    bool l2_ok;                 /* empty-L2 branch + intact scan */
    bool destroy_ok, freed;
    bool ttbr_ok;               /* every round restored boot roots */
    uint64_t esr, far;
};

static void umm_read_ttbrs(uint64_t *ttbr0, uint64_t *ttbr1)
{
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(*ttbr0), "=r"(*ttbr1));
}

static bool umm_ctx_ok(struct umm_run *run, const pt_regs_t *regs)
{
    uint64_t ttbr0, ttbr1;
    umm_read_ttbrs(&ttbr0, &ttbr1);
    /* Kthread mode enters EL0 with DAIF masked (PSTATE 0x3c0), so the
     * SVC callback runs with IRQs masked — require the I bit SET. */
    return cpu_id() == 0 && (current->flags & PF_KTHREAD) &&
           current != &init_task_union.task && current->pid == run->pid &&
           current->on_cpu && !current->in_schedule &&
           (regs->spsr_el1 & AARCH64_EL0_PROBE_IRQ_MASK) != 0 &&
           aarch64_el0_active_root() == run->root &&
           ttbr0 == run->root_pa && ttbr1 == run->svc_ttbr1;
}

static bool umm_sync_handler(pt_regs_t *regs, uint64_t esr, uint64_t far,
                             void *arg)
{
    struct umm_run *run = arg;
    const uint64_t ec = esr >> 26;
    const uint64_t iss = esr & 0x1ffffff;
    run->esr = esr;
    run->far = far;

    run->ctx_ok = umm_ctx_ok(run, regs);
    if (!run->ctx_ok)
        return false;

    if (run->phase == 1 && ec == 0x15 && iss == 8) {
        /* Valid-arg writes while the root is installed and published:
         * all three must refuse with -EBUSY (IRQs are masked here —
         * the reject path never reaches a page-table primitive). */
        int m = arch_vmm_map_4k_new(run->root, run->stack_pa, UM_SVC_VA,
                                    VM_USER_RW | VM_NO_EXEC);
        int u = arch_vmm_update_4k(run->root, run->data_pa, UM_DATA_VA,
                                   VM_USER_RO | VM_NO_EXEC, NULL, NULL);
        int n = arch_vmm_unmap_4k(run->root, UM_STACK_VA, NULL, NULL);
        run->busy_map = m == -EBUSY;
        run->busy_update = u == -EBUSY;
        run->busy_unmap = n == -EBUSY;
        run->r1 = run->busy_map && run->busy_update && run->busy_unmap &&
                  regs->x1 == run->value + 1;
        return false;
    }
    if (run->phase == 2 && ec == 0x24) {
        unsigned dfsc = iss & 0x3f;
        bool write = (iss & (1u << 6)) != 0;
        run->r2 = write && far == UM_DATA_VA && dfsc == 0x0f &&
                  regs->x1 == run->value + 1;
        return false;
    }
    if (run->phase == 3 && ec == 0x24) {
        unsigned dfsc = iss & 0x3f;
        bool write = (iss & (1u << 6)) != 0;
        run->r3 = !write && far == UM_DATA_VA &&
                  dfsc >= 0x04 && dfsc <= 0x07;
        return false;
    }
    return false;
}

static int umm_run_round(struct umm_run *run, const char *entry,
                         unsigned phase)
{
    uint64_t b0, b1, a0, a1;
    umm_read_ttbrs(&b0, &b1);
    run->svc_ttbr1 = b1;
    run->phase = phase;
    struct aarch64_el0_start start = {
        run->root, UM_CODE_VA + (uint64_t)(entry - umm_probe_start),
        UM_STACK_TOP, UM_DATA_VA,
    };
    int rc = aarch64_el0_run_kthread(start, umm_sync_handler, run);
    run->phase = 0;
    umm_read_ttbrs(&a0, &a1);
    if (b0 != a0 || b1 != a1)
        run->ttbr_ok = false;
    return rc;
}

static uint64_t umm_worker(uint64_t arg)
{
    struct umm_run *run = (void *)(uintptr_t)arg;
    uint64_t baseline = pmm_probe_4k_allocated_count();

    if (aarch64_user_mm_create(&run->mm) != 0 || !run->mm)
        return -1;
    run->root_pa = (uint64_t)(uintptr_t)run->mm->pgdir;
    run->root = (uint64_t *)Phy_To_Virt(run->root_pa);
    run->ttbr_ok = true;

    run->code_pa = alloc_4k_page();
    run->data_pa = alloc_4k_page();
    run->stack_pa = alloc_4k_page();
    if (!run->code_pa || !run->data_pa || !run->stack_pa)
        goto fail;
    __builtin_memset((void *)Phy_To_Virt(run->stack_pa), 0, PAGE_4K_SIZE);

    /* Copy the relocation-free probe, then clean its kernel copy the
     * way user_elf.c does (dc cvau per line, then ic iallu after the
     * mappings exist). */
    size_t len = (size_t)(umm_probe_end - umm_probe_start);
    unsigned char *code = (unsigned char *)Phy_To_Virt(run->code_pa);
    __builtin_memcpy(code, umm_probe_start, len);
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4UL << ((ctr >> 16) & 0xf);
    for (size_t i = 0; i < PAGE_4K_SIZE; i += line)
        __asm__ volatile("dc cvau, %0" :: "r"(code + i) : "memory");

    *(uint64_t *)Phy_To_Virt(run->data_pa) = run->value;

    /* All mappings through the M3 entries on the mm-owned root. */
    if (arch_vmm_map_4k_new(run->root, run->code_pa, UM_CODE_VA,
                            VM_USER_RO) != 0 ||                    /* RO/X */
        arch_vmm_map_4k_new(run->root, run->data_pa, UM_DATA_VA,
                            VM_USER_RW | VM_NO_EXEC) != 0 ||       /* RW/NX */
        arch_vmm_map_4k_new(run->root, run->stack_pa, UM_STACK_VA,
                            VM_USER_RW | VM_NO_EXEC) != 0)
        goto fail;
    __asm__ volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");

    if (umm_run_round(run, umm_probe_r1, 1) != 0 || !run->r1)
        goto fail;

    /* Same VA, same PA, demoted to RO/NX. */
    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    if (arch_vmm_update_4k(run->root, run->data_pa, UM_DATA_VA,
                           VM_USER_RO | VM_NO_EXEC, &old_pa,
                           &old_vm) != 0 ||
        old_pa != run->data_pa ||
        (old_vm & VM_WRITE) == 0 || (old_vm & VM_NO_EXEC) == 0)
        goto fail;

    if (umm_run_round(run, umm_probe_r2, 2) != 0 || !run->r2)
        goto fail;

    /* Unmap and verify the handed-back PA before the translation-fault
     * round. */
    uint64_t got_pa = 0;
    uint32_t got_vm = 0;
    if (arch_vmm_unmap_4k(run->root, UM_DATA_VA, &got_pa,
                          &got_vm) != 0 || got_pa != run->data_pa)
        goto fail;
    if (arch_vmm_query_4k(run->root, UM_DATA_VA, NULL, NULL) != -ENOENT)
        goto fail;

    if (umm_run_round(run, umm_probe_r3, 3) != 0 || !run->r3)
        goto fail;

    /* ── Thread context (IRQs enabled, root detached) ─────────────── */
    free_4k_page(run->data_pa);      /* data leaf already unmapped */
    run->data_pa = 0;

    int rc;
    rc = arch_vmm_map_4k_new(run->root, run->code_pa, UM_CODE_VA,
                             VM_USER_RO);
    bool dup_eexist = rc == -EEXIST;
    rc = arch_vmm_map_4k_new(run->root, run->code_pa, UM_KVA, VM_USER_RO);
    bool kern_einval = rc == -EINVAL;
    rc = arch_vmm_map_4k_new(run->root, run->data_pa, UM_SVC_VA,
                             VM_USER_RW | VM_NO_EXEC);
    bool unalloc_einval = rc == -EINVAL;      /* data_pa freed above */
    rc = arch_vmm_map_4k_new(run->root, run->stack_pa, UM_SVC_VA,
                             VM_USER_RW);
    bool bare_einval = rc == -EINVAL;
    rc = arch_vmm_update_4k(run->root, run->code_pa, UM_STACK_VA,
                            VM_USER_RO | VM_NO_EXEC, NULL, NULL);
    bool diffpa_einval = rc == -EINVAL;
    rc = arch_vmm_update_4k(run->root, run->stack_pa, UM_SVC_VA,
                            VM_USER_RO | VM_NO_EXEC, NULL, NULL);
    bool emptyslot_enoent = rc == -ENOENT;
    run->matrix_ok = dup_eexist && kern_einval && unalloc_einval &&
                     bare_einval && diffpa_einval && emptyslot_enoent;

    /* Independent L2 branch: build then tear down, leaving empty
     * tables in a branch the destroy scan walks BEFORE the branch
     * that still holds leaves. */
    uint64_t scratch_pa = alloc_4k_page();
    bool l2 = scratch_pa != 0 &&
              arch_vmm_map_4k_new(run->root, scratch_pa, UM_L2X_VA,
                                  VM_USER_RW | VM_NO_EXEC) == 0;
    got_pa = 0;
    l2 = l2 && arch_vmm_unmap_4k(run->root, UM_L2X_VA, &got_pa,
                                 NULL) == 0 && got_pa == scratch_pa &&
         arch_vmm_query_4k(run->root, UM_L2X_VA, NULL, NULL) == -ENOENT;
    free_4k_page(scratch_pa);

    /* Destroy with leaves → -EBUSY; the empty branch in front must
     * stay intact (allocation count unchanged across the refusal). */
    uint64_t busy_before = pmm_probe_4k_allocated_count();
    int busy_rc = aarch64_user_mm_destroy(run->mm);
    uint64_t busy_after = pmm_probe_4k_allocated_count();
    run->l2_ok = l2 && busy_rc == -EBUSY && busy_before == busy_after;

    /* Both branches still operable after the refusal. */
    run->l2_ok = run->l2_ok &&
                 arch_vmm_query_4k(run->root, UM_CODE_VA, NULL, NULL) == 0 &&
                 arch_vmm_unmap_4k(run->root, UM_L2X_VA, NULL, NULL) == -ENOENT;

    /* Clean up the remaining leaves, then destroy for real. */
    bool unmaps = arch_vmm_unmap_4k(run->root, UM_CODE_VA, &got_pa,
                                    NULL) == 0 && got_pa == run->code_pa &&
                  arch_vmm_unmap_4k(run->root, UM_STACK_VA, &got_pa,
                                    NULL) == 0 && got_pa == run->stack_pa;
    free_4k_page(run->code_pa);
    free_4k_page(run->stack_pa);
    run->code_pa = run->stack_pa = 0;

    run->destroy_ok = unmaps && aarch64_user_mm_destroy(run->mm) == 0;
    run->mm = NULL;
    run->freed = pmm_probe_4k_allocated_count() == baseline;
    run->rc = 0;
    return 0;

fail:
    if (run->data_pa) free_4k_page(run->data_pa);
    if (run->code_pa) free_4k_page(run->code_pa);
    if (run->stack_pa) free_4k_page(run->stack_pa);
    if (run->mm) aarch64_user_mm_destroy(run->mm);
    return -1;
}

int test_aarch64_user_mm(void)
{
    if (dtb_cpu_count() != 1) {
        log_err("M4: use SMP=1 for the user-mm acceptance probe\n");
        return -1;
    }

    static struct umm_run run;
    run = (struct umm_run){ .rc = -1, .value = UM_VALUE };

    run.pid = kernel_thread_on_cpu(umm_worker, (uintptr_t)&run, 0);
    if (run.pid > 0)
        kthread_yield();

    bool ok = run.pid > 0 && run.rc == 0 && run.ctx_ok && run.ttbr_ok &&
              run.r1 && run.busy_map && run.busy_update && run.busy_unmap &&
              run.r2 && run.r3 && run.matrix_ok && run.l2_ok &&
              run.destroy_ok && run.freed;
    if (!ok)
        log_err("M4: rc=%d ctx=%u ttbr=%u r1=%u busy=%u/%u/%u r2=%u r3=%u "
                "matrix=%u l2=%u destroy=%u freed=%u esr=%lx far=%lx\n",
                run.rc, run.ctx_ok, run.ttbr_ok, run.r1, run.busy_map,
                run.busy_update, run.busy_unmap, run.r2, run.r3,
                run.matrix_ok, run.l2_ok, run.destroy_ok, run.freed,
                run.esr, run.far);
    return ok ? 0 : -1;
}

#endif
