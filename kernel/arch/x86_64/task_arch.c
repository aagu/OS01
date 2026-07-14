#include <kernel/task.h>
#include <kernel/arch/mmu.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/thread.h>

void arch_task_init_platform(void)
{
    // Save current page table base (set up by head.S / EFI stub).
    init_mm.pml4 = (uint64_t *)arch_get_page_table();
    init_thread.cr3 = (uint64_t)init_mm.pml4;

    // Program BSP TSS with kernel stack pointers and IST entries.
    set_tss64(init_thread.rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    init_tss[0].rsp0 = init_thread.rsp0;
}
