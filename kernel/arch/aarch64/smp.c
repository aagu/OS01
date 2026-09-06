/* BSP publication for firmware-managed PSCI secondaries.
 *
 * All per-CPU metadata uses its low identity VA, before and after MMU-on.
 * Initialize and clean once before the first CPU_ON; afterward the coherent
 * online/go fields belong to the release/acquire ACK/command protocol.
 */
#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/aarch64/dtb.h>
#include "aarch64_percpu.h"

static bool boot_published;

static void clean_to_poc(uint64_t start, uint64_t end, uint64_t line_size)
{
    uint64_t line = start & ~(line_size - 1);
    for (; line < end; line += line_size) {
        __asm__ __volatile__("dc cvac, %0" :: "r"(line) : "memory");
    }
}

int aarch64_smp_publish_boot(const struct aarch64_topology *topology)
{
    uint64_t bsp, ctr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(bsp));
    bsp &= AARCH64_MPIDR_AFFINITY_MASK;
    if (boot_published || !topology || !topology->cpu_count ||
        topology->cpu_count > NR_CPUS || topology->mpidr[0] != bsp) {
        return -1;
    }

    uint64_t slots_addr = aarch64_percpu_slot_addr(0);
    aarch64_boot_percpu_t *slots = (aarch64_boot_percpu_t *)slots_addr;
    uint64_t table_addr = aarch64_dtb_mpidr_table_addr();
    uint64_t *mpidrs = (uint64_t *)table_addr;
    uint64_t count_addr = aarch64_dtb_cpu_count_addr();

    /* The parser already put the BSP first. Assembly consumes this same
     * order and verifies context_id against both the slot and MPIDR table.
     * Initialize inactive slots as well; never repurpose them after start. */
    for (uint32_t i = 0; i < NR_CPUS; ++i) {
        uint64_t mpidr = i < topology->cpu_count ? topology->mpidr[i] : 0;
        slots[i].self = slots_addr + i * sizeof(*slots);
        slots[i].cpu_id = i;
        slots[i].pad0 = 0;
        slots[i].mpidr = mpidr;
        slots[i].stack = aarch64_boot_stack_top_addr(i);
        slots[i].online = i == 0 ? 1 : 0;
        slots[i].go = AARCH64_BOOT_GO_WAIT;
        slots[i].reserved = 0;
        mpidrs[i] = mpidr;
    }
    *(uint32_t *)count_addr = topology->cpu_count;

    /* CTR_EL0.DminLine gives log2(words per smallest data cache line).
     * APs begin with caches/MMU off, so release stores alone cannot publish
     * these initial writes to them. Clean actual ranges to PoC, then DSB
     * SY before the caller is allowed to issue its first CPU_ON. */
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line_size = UINT64_C(4) << ((ctr >> 16) & 0xf);
    clean_to_poc(aarch64_boot_page_tables_addr(),
                 aarch64_boot_page_tables_end_addr(), line_size);
    clean_to_poc(slots_addr, slots_addr + NR_CPUS * sizeof(*slots), line_size);
    clean_to_poc(table_addr, table_addr + NR_CPUS * sizeof(*mpidrs), line_size);
    clean_to_poc(count_addr, count_addr + sizeof(uint32_t), line_size);
    __asm__ __volatile__("dsb sy" ::: "memory");
    boot_published = true;
    return 0;
}
