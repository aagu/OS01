#include <kernel/arch/x86_64/trampoline.h>
#include <kernel/percpu.h>
#include <kernel/apic.h>
#include <kernel/ipi.h>
#include <kernel/task.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/msr.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <string.h>
#include <stdlib.h>

// Forward declaration from lapic_timer.c
extern void lapic_timer_start(uint32_t freq_hz);

static void ap_init_lapic(void)
{
    uint32_t id = lapic_read(LAPIC_ID) >> 24;
    lapic_write(LAPIC_SVR, APIC_SPURIOUS_VAL);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_LDR, (id << 24));
    lapic_write(LAPIC_LVT_TIMER,   LVT_MASK);
    lapic_write(LAPIC_LVT_LINT0,   LVT_MASK | LVT_DELIVERY_EXTINT);
    lapic_write(LAPIC_LVT_LINT1,   LVT_MASK);
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

    serial_printk("SMP: AP %u (APIC ID %u) reported\n",
                  cpu->cpu_id, cpu->apic_id);

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

    // TSS — copy IST values from BSP template
    cpu->tss = &init_tss[cpu->cpu_id];
    *cpu->tss = init_tss[0];
    cpu->tss->rsp0 = 0xffff800000007c00;
    set_tss64(cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
              cpu->tss->ist1, cpu->tss->ist2, cpu->tss->ist3,
              cpu->tss->ist4, cpu->tss->ist5, cpu->tss->ist6,
              cpu->tss->ist7);

    // Initialise the idle task's thread (allocated by smp_boot_aps).
    // current = idle task (get_current_task via RSP masking).
    memset(cpu->idle->thread, 0, sizeof(thread_t));
    cpu->idle->thread->rsp0 = (uint64_t)cpu->idle + STACK_SIZE;
    cpu->idle->thread->rsp  = (uint64_t)cpu->idle + STACK_SIZE - sizeof(pt_regs_t);
    cpu->idle->thread->fs   = KERNEL_DS;
    cpu->idle->thread->gs   = KERNEL_DS;
    cpu->idle->thread->cr3  = (uint64_t)init_mm.pml4;

    // Start per-CPU scheduling tick
    lapic_timer_start(100);

    cpu->online = 1;
    __sync_synchronize();

    // Record TSC for cross-core warp comparison
    cpu->tsc_boot = rdtsc();

    __asm__ __volatile__("sti");
    serial_printk("SMP: AP %u online (idle pid=%d), entering idle\n",
                  cpu->cpu_id, cpu->idle->pid);

    cpu->scheduler_ok = 1;

    while (1) {
        __asm__ __volatile__("hlt");
        if (cpu->need_resched)
            schedule();
    }
}

// ── Allocate idle task for an AP ────────────────────────────

static task_t *create_idle_task(uint32_t cpu_num)
{
    void *raw = malloc(sizeof(union task_union) + STACK_SIZE);
    if (!raw) return NULL;

    task_t *tsk = (task_t *)(((uint64_t)raw + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    memset(tsk, 0, sizeof(task_t));
    tsk->stack_alloc_base = raw;
    tsk->state    = TASK_RUNNING;
    tsk->flags    = PF_KTHREAD;
    tsk->pid      = 0;
    tsk->priority = 2;
    tsk->counter  = 2;
    tsk->cpu      = cpu_num;
    tsk->addr_limit = 0xffff800000000000UL;

    thread_t *thd = (thread_t *)calloc(1, sizeof(thread_t));
    if (!thd) { kfree(raw); return NULL; }
    tsk->thread = thd;
    list_init(&tsk->list);
    return tsk;
}

void smp_boot_aps(void)
{
    ipi_init();
    uint64_t bsp_cr3 = (uint64_t)get_cr3();

    uintptr_t tramp_size = (uintptr_t)_binary_arch_x86_64_trampoline_bin_end
                         - (uintptr_t)_binary_arch_x86_64_trampoline_bin_start;
    serial_printk("SMP: copying trampoline (%u bytes) to %#010lx\n",
                  (unsigned)tramp_size, (uint64_t)TRAMPOLINE_BASE);
    memcpy((void *)Phy_To_Virt(TRAMPOLINE_BASE),
           _binary_arch_x86_64_trampoline_bin_start, tramp_size);

    trampoline_data_t *tdata = (trampoline_data_t *)Phy_To_Virt(TRAMPOLINE_DATA_BASE);

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (i == 0) continue;
        if (!percpu_data[i].apic_id) continue;

        uint32_t ap_id = percpu_data[i].apic_id;
        serial_printk("SMP: booting AP %u (APIC ID %u)\n", i, ap_id);

        // Create the AP's idle task — the AP runs ap_entry() on
        // this task's kernel stack, so get_current_task() = idle.
        task_t *idle = create_idle_task(i);
        if (!idle) {
            serial_printk("SMP: failed to create idle for AP %u\n", i);
            continue;
        }
        percpu_data[i].idle = idle;
        // Add to the global task list so schedule() can find it.
        list_add_to_before(&init_task_union.task.list, &idle->list);

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
        for (volatile uint32_t d = 0; d < 100000; d++) __asm__ __volatile__("pause");
        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_INIT);
        for (volatile uint32_t d = 0; d < 100000; d++) __asm__ __volatile__("pause");

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_STARTUP | ((uint32_t)TRAMPOLINE_BASE >> 12));
        for (volatile uint32_t d = 0; d < 2000; d++) __asm__ __volatile__("pause");

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_STARTUP | ((uint32_t)TRAMPOLINE_BASE >> 12));

        for (volatile uint32_t t = 0; t < 5000000; t++) {
            if (percpu_data[i].online) break;
            __asm__ __volatile__("pause");
        }
        if (percpu_data[i].online) {
            serial_printk("SMP: AP %u (APIC ID %u) booted successfully\n", i, ap_id);

            // Quick TSC warp check — compare BSP vs AP readings.
            // Not a full point-to-point measurement (no shared spin-wait),
            // but enough to detect gross skew.
            {
                uint64_t bsp_tsc = rdtsc();
                uint64_t ap_tsc  = percpu_data[i].tsc_boot;
                int64_t  diff    = (int64_t)(bsp_tsc - ap_tsc);
                const char *status = (diff > -5000000LL && diff < 5000000LL)
                                     ? "OK" : "WARP";
                serial_printk("SMP: TSC sync AP%u: BSP=%#018lx AP=%#018lx diff=%+ld %s\n",
                              i, bsp_tsc, ap_tsc, (long long)diff, status);
            }
            for (volatile uint32_t d = 0; d < 10000; d++) __asm__ __volatile__("pause");
        } else {
            serial_printk("SMP: AP %u (APIC ID %u) FAILED\n", i, ap_id);
            list_del(&idle->list);
            kfree(idle->thread);
            kfree(idle->stack_alloc_base);
            percpu_data[i].idle = NULL;
        }
    }
}
