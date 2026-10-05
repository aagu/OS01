#include <percpu/percpu.h>
#include <arch/cpu.h>
#include <stddef.h>
#include <string.h>

percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus;

void percpu_install_gs(uint32_t cpu)
{
    arch_set_percpu_base(&percpu_data[cpu]);
}

/* M3 (Task 7): the ipi_ready/tlb_ack_gen tail is excluded from the wipe —
 * ipi_ready has one-shot publication semantics (ipi_ready_publish_and_count)
 * and a re-init must not clobber a published flag. Callers set these fields
 * explicitly. The tail position is pinned by _Static_asserts in percpu.h. */
_Static_assert(offsetof(percpu_t, ipi_ready) == sizeof(percpu_t) - 8,
               "memset-skip tail drifted; fix the wipe size below");

void percpu_init(uint32_t cpu, uint32_t apic_id)
{
    memset(&percpu_data[cpu], 0, sizeof(percpu_t) - 8);
    percpu_data[cpu].cpu_id  = cpu;
    percpu_data[cpu].arch_processor_id = apic_id;
    /* M3.5 Task 25 fix: mark this CPU online. aarch64 never set this
     * (x86 set only percpu_data[0].online from its own boot path), so
     * tlb_shootdown()'s target snapshot (online ∧ ipi_ready ∧ ¬self,
     * memory/tlb.c) was always EMPTY on aarch64 — shootdowns degraded
     * to a local flush and APs kept stale TLB entries, which the
     * production shootdown probe caught as `FAIL ap-read-B`. Release
     * store per the §6.2b dual-state protocol (the consuming BSP
     * acquire-syncs on ipi_ready_count). Setting it here (before
     * ipi_ready publication) is safe: tlb_shootdown additionally
     * requires ipi_ready, which is published only after the AP has
     * IRQs open. */
    __atomic_store_n(&percpu_data[cpu].online, 1, __ATOMIC_RELEASE);
    // Store self-pointer as the first qword so GS:0 yields &percpu_data[cpu]
    percpu_data[cpu].self = (uint64_t)&percpu_data[cpu];
    rbtree_init(&percpu_data[cpu].run_queue);
    percpu_data[cpu].min_vruntime = 0;
    percpu_data[cpu].rq_lock.lock = 1L;
}
