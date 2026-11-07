/* hosttests/mock/x86_smp_boot_sync_runtime.h — machine-boundary mock for
 * the x86 boot-sync wait helper host test.
 *
 * The test case links the REAL kernel/arch/x86_64/smp/boot_sync.c and
 * replaces only the machine pause: the mock counts pauses and, when the
 * case configures a publication point, release-writes the flag the
 * production helper is waiting on — exactly what an AP publishing its
 * readiness does across the real machine.  The bounded acquire polling
 * algorithm under test stays in boot_sync.c.
 */
#ifndef X86_SMP_BOOT_SYNC_TEST_RUNTIME_H
#define X86_SMP_BOOT_SYNC_TEST_RUNTIME_H

#include <stdint.h>

/* Short-circuit <arch/cpu.h>: the production object needs nothing from
 * it except the pause, which the mock replaces below (same guard
 * pattern as ap_work_test_runtime.h). */
#define _ARCH_CPU_H

/* Spy surface — defined in the test TU. */
extern long boot_sync_test_pause_count;
/* -1 = never publish; N = release-write 1 into *boot_sync_test_flag as
 * soon as the pause count reaches N. */
extern long boot_sync_test_publish_at_pause;
extern uint32_t *boot_sync_test_flag;

static inline void boot_sync_test_pause(void)
{
    boot_sync_test_pause_count++;
    if (boot_sync_test_publish_at_pause >= 0 &&
        boot_sync_test_pause_count >= boot_sync_test_publish_at_pause &&
        boot_sync_test_flag != 0) {
        __atomic_store_n(boot_sync_test_flag, 1u, __ATOMIC_RELEASE);
    }
}

/* The production code calls arch_cpu_pause(); rename call sites onto
 * the mock (the real x86_64 static inline is only renamed, never
 * called). */
#define arch_cpu_pause() boot_sync_test_pause()

#endif /* X86_SMP_BOOT_SYNC_TEST_RUNTIME_H */
