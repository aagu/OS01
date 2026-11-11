#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/mmu.h>
#include <stddef.h>

extern void aarch64_el0_enter(uint64_t pc, uint64_t sp, uint64_t x0);

/* Only live on the BSP, before GIC/timer and AP startup. */
static aarch64_el0_handler active_handler;
static void *active_arg;
static uint64_t *active_root;

uint64_t *aarch64_el0_active_root(void)
{
    return active_root;
}

static void switch_ttbr0(uint64_t raw)
{
    /* ASID 0 throughout this probe. Invalidate all local EL1&0 entries
     * (including global ones) before using the replacement root. */
    __asm__ volatile("dsb ishst\n\t"
                     "msr ttbr0_el1, %0\n\tisb\n\t"
                     "tlbi vmalle1\n\tdsb ish\n\tisb"
                     :: "r"(raw) : "memory");
}

int aarch64_el0_run(uint64_t *root, uint64_t pc, uint64_t sp, uint64_t x0,
                   aarch64_el0_handler handler, void *arg)
{
    uintptr_t kernel_sp;
    __asm__ volatile("mov %0, sp" : "=r"(kernel_sp));
    if (!root || !handler || active_handler || !aarch64_vmm_pre_smp() ||
        kernel_sp < ARCH_PAGE_OFFSET)
        return -1;
    uint64_t root_pa = (uintptr_t)root - ARCH_PAGE_OFFSET;
    if (aarch64_pt_root_is_published(&root_pa))
        return -1;
    uint64_t boot_ttbr0, saved_sp, daif;
    __asm__ volatile("mrs %0, ttbr0_el1\n\t"
                     "mrs %1, sp_el0\n\t"
                     "mrs %2, daif\n\tmsr daifset, #0xf"
                     : "=r"(boot_ttbr0), "=r"(saved_sp), "=r"(daif) :: "memory");
    if (!aarch64_pt_root_publish(root_pa)) {
        __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
        return -1;
    }

    active_arg = arg;
    active_handler = handler;
    switch_ttbr0(root_pa);
    active_root = root;
    aarch64_el0_enter(pc, sp, x0);
    active_root = NULL;
    switch_ttbr0(boot_ttbr0);
    aarch64_pt_root_unpublish_local(root_pa);
    active_handler = NULL;
    active_arg = NULL;
    __asm__ volatile("msr sp_el0, %0\n\tmsr daif, %1"
                     :: "r"(saved_sp), "r"(daif) : "memory");
    return 0;
}

/* Called only by the lower-AArch64 synchronous vector. */
bool aarch64_el0_sync(pt_regs_t *regs, uint64_t esr, uint64_t far)
{
    if (!active_handler || (regs->spsr_el1 & 0xf) != 0) {
        vmm_gate_violation("unexpected EL0 sync");
        return false;
    }
    return active_handler(regs, esr, far, active_arg);
}
#endif
