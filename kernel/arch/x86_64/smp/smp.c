#include <arch/x86_64/trampoline.h>
#include <arch/x86_64/smp/boot_sync.h>
#include <percpu/percpu.h>
#include <intr/apic.h>
#include <intr/ipi.h>
#include <sched/task.h>
#include <sched/resource.h>
#include <arch/cpu.h>
#include <arch/x86_64/tss.h>
#include <arch/irq.h>
#include <arch/x86_64/gate.h>
#include <arch/msr.h>
#include <sched/accounting.h>
#include <core/debug.h>
#include <core/panic.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <string.h>
#include <stdlib.h>

// Forward declaration from lapic_timer.c
extern bool lapic_timer_start(uint32_t freq_hz);

// 两侧握手自旋的统一上限（BSP 等 AP、AP 等 BSP 用同一宏）。
#define SYNC_SPIN_LIMIT 1000000u

static void ap_init_lapic(void)
{
    uint32_t id = lapic_read(LAPIC_ID) >> 24;
    lapic_write(LAPIC_SVR, APIC_SPURIOUS_VAL);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_LDR, (id << 24));
    lapic_write(LAPIC_LVT_TIMER,   LVT_MASK);
    lapic_write(LAPIC_LVT_LINT0,   LVT_MASK | LVT_DELIVERY_EXTINT);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASK);
    lapic_write(LAPIC_LVT_ERROR,   LVT_MASK);
    lapic_write(LAPIC_LVT_PERF,    LVT_MASK);
    lapic_write(LAPIC_LVT_THERMAL, LVT_MASK);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_EOI, 0);
}

void ap_entry(void)
{
    percpu_t *cpu = this_cpu();

    debug_sched("SMP: AP %u (APIC ID %u) reported\n",
                  cpu->cpu_id, cpu->arch_processor_id);

    uint64_t apic_msr = rdmsr(IA32_APIC_BASE);
    if (!(apic_msr & APIC_BASE_ENABLE)) {
        wrmsr(IA32_APIC_BASE, apic_msr | APIC_BASE_ENABLE);
    }
    ap_init_lapic();

    // ── Load the kernel's GDT and IDT ──────────────────
    // The trampoline loaded its own local GDT and never set
    // up IDTR.  We must switch to the kernel descriptor tables
    // before enabling interrupts, or the CPU will fault on the
    // first interrupt delivery.
    {
        struct { uint16_t limit; uint64_t base; }
            __attribute__((packed)) gdtr;
        extern struct desc_struct GDT_Table[];
        gdtr.limit = 0x367;   // GDT size - 1
        gdtr.base  = (uint64_t)GDT_Table;
        __asm__ __volatile__("lgdt %0" :: "m"(gdtr) : "memory");
    }
    {
        struct { uint16_t limit; uint64_t base; }
            __attribute__((packed)) idtr;
        extern struct gate_struct IDT_Table[];
        idtr.limit = 512 * 16 - 1;
        idtr.base  = (uint64_t)IDT_Table;
        __asm__ __volatile__("lidt %0" :: "m"(idtr) : "memory");
    }
    // Reload segment selectors to use the kernel GDT.
    // The trampoline used CS=0x18 and DS=0x20, which map to
    // uninitialised NULL entries in the kernel GDT.  We must
    // switch all of them before enabling interrupts.
    __asm__ __volatile__(
        "movw %0, %%ds \n\t"
        "movw %0, %%es \n\t"
        "movw %0, %%ss \n\t"
        // Reload CS via far return — push selector:RIP and lretq
        "pushq $0x08 \n\t"              // KERNEL_CS
        "leaq   1f(%%rip), %%rax \n\t"
        "pushq  %%rax \n\t"
        "lretq \n\t"
        "1: \n\t"
        :: "r"((uint16_t)KERNEL_DS) : "memory", "rax"
    );

    // TSS — per-CPU, installed by smp_boot_aps via set_tss_descriptor.
    cpu->tss = &init_tss[cpu->cpu_id];

    // Initialise the idle task's thread (allocated by smp_boot_aps).
    // current = idle task (get_current_task via RSP masking).
    memset(cpu->idle->thread, 0, sizeof(thread_t));
    cpu->idle->thread->rsp0 = (uint64_t)cpu->idle + STACK_SIZE;
    cpu->idle->thread->rsp  = (uint64_t)cpu->idle + STACK_SIZE - sizeof(pt_regs_t);
    cpu->idle->thread->fs   = KERNEL_DS;
    cpu->idle->thread->gs   = KERNEL_DS;
    cpu->idle->thread->cr3  = (uint64_t)init_mm.pgdir;

    // Seed the per-CPU TSS with idle-task ring-0 stack + IST values
    // copied from the BSP template.
    cpu->tss->rsp0 = cpu->idle->thread->rsp0;
    cpu->tss->rsp1 = init_tss[0].rsp1;
    cpu->tss->rsp2 = init_tss[0].rsp2;
    set_tss64(cpu->tss_hw,
              cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    // Load the per-CPU Task Register.  Each CPU has its own TSS
    // descriptor in the shared GDT at slot 8+2*cpu_id (installed by
    // smp_boot_aps).  Without this, the CPU would use the BSP's TSS
    // and corrupt the BSP's ring-0 stack pointer on every context
    // switch.
    load_TR((8 + cpu->cpu_id * 2));

    // NMI ignores IF. Keep LINT inputs masked until the kernel GDT/IDT,
    // selectors and per-CPU TSS/IST are usable (GS was set by trampoline).
    // Publish online readiness only after firmware NMI routing is active.
    lapic_configure_nmi();

    // Start per-CPU scheduling tick.  Monitor plan Task 2: the AP's
    // accounting lifecycle is published HERE, exactly once, from the
    // REAL lapic_timer_start() return value — a failed AP timer stays
    // valid=0 (unavailable) and must never be flipped to valid by any
    // other per-cpu callback (the BSP-side subsys callback skips the
    // publish entirely).
    {
        bool tick_started = lapic_timer_start(100);
        sched_accounting_cpu_start(cpu->cpu_id, 100, tick_started);
    }

    /* BOOT-1: the AP publishes its own runtime online — LAPIC, GDT/IDT,
     * selectors, idle/thread, TSS/TR, NMI routing and the local tick
     * attempt above are all complete here.  Release store: the BSP's
     * bounded wait acquire-reads this exact publication.  Publishing
     * BEFORE the TSC handshake avoids BSP/AP mutual spin (spec §5.2). */
    __atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);
    uint32_t w = 0;
    while (w++ < SYNC_SPIN_LIMIT &&
           !__atomic_load_n(&cpu->tsc_sync_go, __ATOMIC_ACQUIRE))  // 2. 有界等 BSP
        arch_cpu_pause();
    if (__atomic_load_n(&cpu->tsc_sync_go, __ATOMIC_ACQUIRE)) {
        cpu->tsc_boot = rdtsc();         // 3. 先写采样值 …
        __atomic_store_n(&cpu->tsc_sampled, 1, __ATOMIC_RELEASE);  // 4. … 再发布完成
    }

    arch_local_irq_enable();
    /* M3 Task 12: AP publishes ipi_ready after IDT/handler are live
     * (lidt above) and IRQs are unmasked (arch_local_irq_enable above).
     * No smp_starting gate on x86 (aarch64-only sequencing device). */
    ipi_ready_publish_ap(cpu->cpu_id);
    debug_sched("SMP: AP %u online (idle pid=%d), entering idle\n",
                  cpu->cpu_id, cpu->idle->pid);

    /* RELEASE: paired with the acquire loads in the constructor /
     * placement / balance / schedule readers (P1 spec §6). */
    __atomic_store_n(&cpu->scheduler_ok, 1, __ATOMIC_RELEASE);

    while (1) {
        arch_cpu_halt();
        if (__atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE)) {
            schedule();
            // schedule() returns with IRQs disabled; hlt() needs IF=1.
            arch_local_irq_enable();
        }
    }
}

// ── Allocate idle task for an AP ────────────────────────────

static task_t *create_idle_task(uint32_t cpu_num)
{
    void *raw = malloc(sizeof(union task_union) + STACK_SIZE);
    if (!raw) return NULL;

    task_t *tsk = (task_t *)(((uint64_t)raw + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    memset(tsk, 0, sizeof(task_t));
    task_nofile_init(tsk);
    tsk->stack_alloc_base = raw;
    tsk->state    = TASK_RUNNING;
    tsk->flags    = PF_KTHREAD;
    tsk->pid      = 0;
    tsk->priority = 2;
    tsk->counter  = 2;
    tsk->cpu      = cpu_num;
    /* Thermal plan Task 2: AP idle may only run on its own CPU.
     * Pre-publication channel ensures the picker can't place it
     * on any other cpu even if the runqueue mask is briefly empty.
     */
    tsk->allowed_cpus = 1ULL << cpu_num;
    tsk->addr_limit = 0xffff800000000000UL;

    thread_t *thd = (thread_t *)calloc(1, sizeof(thread_t));
    if (!thd) { kfree(raw); return NULL; }
    tsk->thread = thd;
    list_init(&tsk->list);
    return tsk;
}

/* BOOT-1: local noreturn fatal helper.  A failed AP boot stops the BSP
 * for good — no continuation into task_init, no PID 1.  Any resource an
 * AP may already be using (idle stack, thread, task-list node, TSS,
 * trampoline data) is NEVER freed or reused after the SIPI went out;
 * fail-closed replaces the old "free and continue" degradation, whose
 * safety against a late AP was unprovable (spec §6). */
static void boot_fatal_cpu(uint32_t cpu, const char *stage) __attribute__((noreturn));
static void boot_fatal_cpu(uint32_t cpu, const char *stage)
{
    log_err("[boot-1] FATAL cpu=%u stage=%s\n", cpu, stage);
    kpanic("BOOT-1: AP boot failed, cpu=%u stage=%s", cpu, stage);
    /* kpanic() halts forever but its shared declaration is not marked
     * noreturn; pin the noreturn contract locally. */
    __builtin_unreachable();
}

void smp_boot_aps(void)
{
    ipi_init();
    /* M3 Task 12: BSP publishes ipi_ready only after the TLB IPI handler
     * is registered (ipi_init above) and before any AP exists to target.
     * See ipi_ready_publish_bsp() in smp/boot.c. */
    ipi_ready_publish_bsp();
    uint64_t bsp_cr3 = (uint64_t)get_cr3();

    uintptr_t tramp_size = (uintptr_t)_binary_arch_x86_64_trampoline_bin_end
                         - (uintptr_t)_binary_arch_x86_64_trampoline_bin_start;
    debug_sched("SMP: copying trampoline (%u bytes) to %#010lx\n",
                  (unsigned)tramp_size, (uint64_t)TRAMPOLINE_BASE);
    memcpy((void *)Phy_To_Virt(TRAMPOLINE_BASE),
           _binary_arch_x86_64_trampoline_bin_start, tramp_size);

    trampoline_data_t *tdata = (trampoline_data_t *)Phy_To_Virt(TRAMPOLINE_DATA_BASE);

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (i == 0) continue;  // skip BSP (already running)

        uint32_t ap_id = percpu_data[i].arch_processor_id;
        debug_sched("SMP: booting AP %u (APIC ID %u)\n", i, ap_id);

        /* BOOT-1 stage 1: the target must start with no publication.
         * Residual online/scheduler_ok is a boot protocol error — it is
         * never cleared to mask it (spec §6 step 1). */
        if (__atomic_load_n(&percpu_data[i].online, __ATOMIC_ACQUIRE) != 0 ||
            __atomic_load_n(&percpu_data[i].scheduler_ok, __ATOMIC_ACQUIRE) != 0)
            boot_fatal_cpu(i, "invariant");

        // Create the AP's idle task — the AP runs ap_entry() on
        // this task's kernel stack, so get_current_task() = idle.
        // Allocation failure happens BEFORE the SIPI, so nothing was
        // exposed to the AP yet; fatal here is safe (spec §6).
        task_t *idle = create_idle_task(i);
        if (!idle)
            boot_fatal_cpu(i, "prepare");
        percpu_data[i].idle = idle;
        // Add to the global task list so schedule() can find it.
        // Must use task_list_add() which holds task_list_lock:
        // previously-booted APs may already be running schedule()
        // and scanning this list concurrently.
        task_list_add(idle);

        // ── Per-CPU TSS descriptor in the shared GDT ──────────
        // CPU i uses GDT entries [8+2i, 9+2i] → selector (8+2i)<<3.
        // Without per-CPU TSS areas each core's context-switch path
        // would overwrite every other core's ring-0 stack pointer.
        set_tss_descriptor(8 + i * 2, &init_tss[i]);
        percpu_data[i].tss_hw = &init_tss[i];   // AP uses its own TSS

        /* Trampoline data fill — the ONLY fill site.  BOOT-1: the next
         * AP's fill happens at the top of the next iteration, i.e. only
         * after this AP published scheduler_ok (stage 3 below), so the
         * still-running trampoline handshake can never be overwritten. */
        tdata->cr3     = bsp_cr3;
        tdata->gs_base = (uint64_t)&percpu_data[i];
        tdata->stack   = (uint64_t)idle + STACK_SIZE;
        tdata->entry   = (uint64_t)ap_entry;
        tdata->apic_id = ap_id;
        tdata->cpu_id  = i;
        __sync_synchronize();

        // INIT-SIPI-SIPI
        uint32_t icr_high = ap_id << 24;
        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT);
        for (volatile uint32_t d = 0; d < 100000; d++) arch_cpu_pause();
        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_INIT);
        for (volatile uint32_t d = 0; d < 100000; d++) arch_cpu_pause();

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_STARTUP | ((uint32_t)TRAMPOLINE_BASE >> 12));
        for (volatile uint32_t d = 0; d < 2000; d++) arch_cpu_pause();

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_STARTUP | ((uint32_t)TRAMPOLINE_BASE >> 12));

        /* BOOT-1 stage 2: bounded acquire wait for the AP's OWN online
         * publication.  A constructed CPU is not a running CPU, so the
         * pre-SIPI construction cannot satisfy this wait.  The helper's
         * budget is a polling budget, not a wall-clock deadline. */
        if (!x86_smp_wait_u32_acquire(&percpu_data[i].online,
                                      X86_SMP_READY_SPIN_LIMIT))
            boot_fatal_cpu(i, "online");

        debug_sched("SMP: AP %u (APIC ID %u) booted successfully\n", i, ap_id);

        // TSC 握手采样：BSP release 发起、acquire 等 AP 回写，算跨核偏移。
        // tsc_boot 在 sampled 之前由 AP 写入；超时仍按既有策略 offset=0
        // 并告警 —— 采样超时不是启动失败（spec §6 step 4）。
        __atomic_store_n(&percpu_data[i].tsc_sync_go, 1, __ATOMIC_RELEASE);
        uint64_t bsp_tsc = rdtsc();
        {
            /* tsc_sampled is a u64 word, so this advisory wait keeps its
             * own bounded acquire loop (the u32 helper is for the two
             * REQUIRED waits).  Timeout keeps the existing policy:
             * offset=0 + warning — sampling timeout is not a boot
             * failure (spec §6 step 4). */
            uint32_t w = 0;
            while (w++ < SYNC_SPIN_LIMIT &&
                   !__atomic_load_n(&percpu_data[i].tsc_sampled,
                                    __ATOMIC_ACQUIRE))
                arch_cpu_pause();
        }
        if (!__atomic_load_n(&percpu_data[i].tsc_sampled, __ATOMIC_ACQUIRE)) {
            debug_sched("SMP: TSC sync AP%u timeout, offset unset\n", i);
            percpu_data[i].tsc_offset = 0;
        } else {
            uint64_t ap_tsc = percpu_data[i].tsc_boot;
            percpu_data[i].tsc_offset = (int64_t)(bsp_tsc - ap_tsc);
            debug_sched("SMP: TSC sync AP%u: offset=%+ld\n", i,
                        (long)percpu_data[i].tsc_offset);
        }

        /* BOOT-1 stage 3: scheduler_ok gates trampoline reuse — the next
         * AP's trampoline fill only happens after this publication is
         * acquire-observed (spec §4 invariant 6). */
        if (!x86_smp_wait_u32_acquire(&percpu_data[i].scheduler_ok,
                                      X86_SMP_READY_SPIN_LIMIT))
            boot_fatal_cpu(i, "scheduler");

        /* Invariant re-check under acquire: once scheduler_ok was
         * observed, online ∧ ipi_ready must both hold (spec §6 step 5). */
        if (__atomic_load_n(&percpu_data[i].online, __ATOMIC_ACQUIRE) != 1 ||
            __atomic_load_n(&percpu_data[i].ipi_ready, __ATOMIC_ACQUIRE) != 1)
            boot_fatal_cpu(i, "invariant");

        log_info("[boot-1] ready cpu=%u online=1 ipi_ready=1 scheduler_ok=1\n",
                 i);

        for (volatile uint32_t d = 0; d < 10000; d++) arch_cpu_pause();
    }

    /* BOOT-1: all discovered CPUs satisfied the boot contract (BSP
     * included — the loop above is fatal on any AP failure).  Emitted
     * exactly once; N includes the BSP, so a single-CPU boot has no AP
     * ready lines but still gets this line (spec §6). */
    log_info("[boot-1] complete discovered=%u ready=%u\n", num_cpus, num_cpus);
}
