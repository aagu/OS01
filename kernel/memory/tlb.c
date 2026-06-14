#include <kernel/vmm.h>
#include <kernel/ipi.h>
#include <kernel/percpu.h>
#include <kernel/printk.h>

// ── TLB shootdown for SMP ──────────────────────────────────
//
// When a CPU modifies shared kernel page tables (kernel_map,
// PML4 entries 256–511), other CPUs may still cache stale
// TLB mappings.  This function:
//
//   1. Sets tlb_wanted = 1 on every OTHER online CPU
//   2. Sends IPI_TLB to each of them
//   3. Flushes the local TLB (CR3 reload)
//   4. Spins until every target has incremented tlb_ack
//
// If num_cpus ≤ 1 the IPI loop is skipped and only a local
// flush_tlb() is performed — safe to call during early boot.

void tlb_shootdown(void)
{
    uint32_t me = cpu_id();

    // Fast path: single CPU (early boot or -smp 1)
    if (num_cpus <= 1)
        goto local_only;

    // 1. Flag every other online CPU
    for (uint32_t i = 0; i < num_cpus; i++) {
        if (i == me || !percpu_data[i].online)
            continue;
        percpu_data[i].tlb_wanted = 1;
    }
    __sync_synchronize();

    // 2. Send TLB shootdown IPI to all other online CPUs
    ipi_broadcast(IPI_VECTOR_TLB, /*exclude_self=*/1);

    // 3. Flush our own TLB
    flush_tlb();

    // 4. Wait for all targets to acknowledge
    for (uint32_t i = 0; i < num_cpus; i++) {
        if (i == me || !percpu_data[i].online)
            continue;

        uint32_t timeout = 100000;
        while (percpu_data[i].tlb_ack == 0 && --timeout > 0)
            __asm__ __volatile__("pause");

        if (timeout == 0) {
            serial_printk("TLB shootdown: CPU %u did not ACK\n", i);
        }

        percpu_data[i].tlb_ack = 0;
        percpu_data[i].tlb_wanted = 0;
    }

    return;

local_only:
    flush_tlb();
}
