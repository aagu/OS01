#include <kernel/arch/x86_64/trampoline.h>
#include <kernel/percpu.h>
#include <kernel/apic.h>
#include <kernel/ipi.h>
#include <kernel/task.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/msr.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <string.h>
#include <stdlib.h>

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

    cpu->tss = &init_tss[cpu->cpu_id];
    *cpu->tss = init_tss[0];
    cpu->tss->rsp0 = 0xffff800000007c00;

    set_tss64(cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
              cpu->tss->ist1, cpu->tss->ist2, cpu->tss->ist3,
              cpu->tss->ist4, cpu->tss->ist5, cpu->tss->ist6,
              cpu->tss->ist7);

    cpu->online = 1;
    __sync_synchronize();

    // LAPIC timer started by BSP after all APs are up — avoids
    // perturbing the shared ICR during INIT-SIPI sequences.

    __asm__ __volatile__("sti");
    serial_printk("SMP: AP %u online, entering idle\n", cpu->cpu_id);

    while (1) {
        __asm__ __volatile__("hlt");
        if (cpu->need_resched && cpu->scheduler_ok)
            schedule();
    }
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
        if (i == 0)
            continue;
        if (!percpu_data[i].apic_id)
            continue;

        uint32_t ap_id = percpu_data[i].apic_id;
        serial_printk("SMP: booting AP %u (APIC ID %u)\n", i, ap_id);

        void *tmp_stack = malloc(STACK_SIZE);
        if (!tmp_stack) {
            serial_printk("SMP: failed to allocate stack for AP %u\n", i);
            continue;
        }

        tdata->cr3     = bsp_cr3;
        tdata->gs_base = (uint64_t)&percpu_data[i];
        tdata->stack   = (uint64_t)tmp_stack + STACK_SIZE;
        tdata->entry   = (uint64_t)ap_entry;
        tdata->apic_id = ap_id;
        tdata->cpu_id  = i;

        __sync_synchronize();

        // INIT-SIPI-SIPI
        uint32_t icr_high = ap_id << 24;
        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT);

        for (volatile uint32_t d = 0; d < 100000; d++)
            __asm__ __volatile__("pause");

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW, ICR_DELIVERY_INIT);

        for (volatile uint32_t d = 0; d < 100000; d++)
            __asm__ __volatile__("pause");

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW,
                    ICR_DELIVERY_STARTUP | ((uint32_t)TRAMPOLINE_BASE >> 12));

        for (volatile uint32_t d = 0; d < 2000; d++)
            __asm__ __volatile__("pause");

        lapic_write(LAPIC_ICR_HIGH, icr_high);
        lapic_write(LAPIC_ICR_LOW,
                    ICR_DELIVERY_STARTUP | ((uint32_t)TRAMPOLINE_BASE >> 12));

        for (volatile uint32_t t = 0; t < 5000000; t++) {
            if (percpu_data[i].online)
                break;
            __asm__ __volatile__("pause");
        }

        if (percpu_data[i].online) {
            serial_printk("SMP: AP %u (APIC ID %u) booted successfully\n",
                          i, ap_id);
            // Give the AP a moment to finish its LAPIC init before
            // we touch the ICR again for the next AP.
            for (volatile uint32_t d = 0; d < 10000; d++)
                __asm__ __volatile__("pause");
        } else {
            serial_printk("SMP: AP %u (APIC ID %u) FAILED to come online\n",
                          i, ap_id);
            kfree(tmp_stack);
        }
    }
}
