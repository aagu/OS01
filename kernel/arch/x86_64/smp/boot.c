// kernel/arch/x86_64/smp/boot.c
//
// x86_64 SMP bring-up helpers — extracted from kernel/core/main.c
// as part of the kernel-main refactor (Task 5).
//
// Owns two ordered phases that previously lived inline in kernel_main:
//
//   x86_64_boot_percpu() — single-CPU MADT traversal with NR_CPUS
//                          drop-out branch, percpu_init() per enabled
//                          APIC entry, BSP (cpu_idx==0) TSS/tss_hw/GS/
//                          online writes in that exact order, and the
//                          num_cpus publication only AFTER the loop
//                          completes.  The "publish at end" ordering
//                          matters: AP spin-ups in the next phase read
//                          num_cpus to know how many APs to start.
//
//   x86_64_boot_aps()    — tick_start() (now this_cpu() is valid for
//                          the BSP) → smp_boot_aps() (INIT-SIPI-SIPI
//                          for every AP) → per-CPU subsystem dispatch
//                          (arch_register_subsys_percpu + subsys_init
//                          _percpu).  No state, no locals.

#include <arch/x86_64/smp_boot.h>

#include <stdint.h>

#include <percpu/percpu.h>
#include <intr/apic.h>
#include <driver/serial.h>
#include <core/printk.h>
#include <arch/thread.h>
#include <time/clockevent.h>
#include <subsys/subsys.h>
#include <arch/subsys.h>
#include <arch/cpu.h>

// Forward declaration — smp_boot_aps() lives in smp.c (same dir).
void smp_boot_aps(void);

void x86_64_boot_percpu(void)
{
    uint32_t cpu_idx = 0;
    for (uint32_t i = 0; i < apic_info.lapic_count; i++) {
        if (!(apic_info.lapics[i].flags & 1))
            continue;

        if (cpu_idx >= NR_CPUS) {
            serial_printk("percpu: APIC id=%u DROPPED (NR_CPUS=%u)\n",
                          apic_info.lapics[i].apic_id, (unsigned)NR_CPUS);
            continue;
        }

        percpu_init(cpu_idx, apic_info.lapics[i].apic_id);

        if (cpu_idx == 0) {
            percpu_data[0].tss = &init_tss[0];
            percpu_data[0].tss_hw = arch_task_boot_state();
            percpu_install_gs(0);
            percpu_data[0].online = 1;
            serial_printk("percpu: BSP  (cpu=%u, apic_id=%u) online\n",
                          cpu_idx, apic_info.lapics[i].apic_id);
        } else {
            serial_printk("percpu: AP   (cpu=%u, apic_id=%u) registered\n",
                          cpu_idx, apic_info.lapics[i].apic_id);
        }
        cpu_idx++;
    }
    serial_printk("percpu: %u CPU(s) registered (%u in MADT)\n",
                  cpu_idx, apic_info.lapic_count);
    num_cpus = cpu_idx;
}

void x86_64_boot_aps(void)
{
    // 显式启动 tick 源：GS base 已装（x86_64_boot_percpu 中的
    // percpu_install_gs(0)），this_cpu() 可用。tick_start 先掩 PIT
    // 再启 LAPIC，失败回退 PIT。
    tick_start();

    smp_boot_aps();

    // per-CPU 子系统二次 init
    arch_register_subsys_percpu();
    subsys_init_percpu();
}
