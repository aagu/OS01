// kernel/arch/x86_64/smp/boot_sync.c
//
// BOOT-1: bounded acquire wait helper used by the x86 BSP two-stage AP
// handshake.  The polling algorithm is deliberately tiny and free of
// kernel dependencies (no current-task read, no allocation, no IRQ
// masking) so the hosttest test_x86_smp_boot_sync can compile this REAL
// object and drive every contract clause with a mocked pause.

#include <arch/x86_64/smp/boot_sync.h>
#include <arch/cpu.h>

bool x86_smp_wait_u32_acquire(const uint32_t *flag, uint32_t budget)
{
    if (!flag || budget == 0)
        return false;

    for (uint32_t round = 0; round < budget; round++) {
        if (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0)
            return true;
        arch_cpu_pause();
    }
    /* The final pause happened; per the contract there is no extra
     * read after it — a publication landing here is not observed. */
    return false;
}
