/* hosttests/mock/boot_cpu_runtime.h — machine-boundary mock for the
 * BOOT-1 percpu construction host test.
 *
 * The test case links the REAL kernel/percpu/percpu.c against the REAL
 * percpu_t (kernel/include/percpu/percpu.h).  The mock replaces only the
 * machine boundary percpu.c can reach: <arch/cpu.h>'s x86_64 branch,
 * whose arch_set_percpu_base() performs an IA32_GS_BASE MSR write.
 * Guard-based replacement (same pattern as ap_work_test_runtime.h):
 * the header guard is predefined so the real machine branch never
 * expands, and the mock provides the one function the construction
 * surface can name.
 *
 * The mock records how the production code touched the boundary so the
 * case can assert percpu_init() never installs a base (installation is
 * the caller's job, per the BOOT-1 construction contract).  It does NOT
 * replicate any construction state machine: the construction algorithm
 * under test stays in percpu.c.
 */
#ifndef BOOT_CPU_TEST_RUNTIME_H
#define BOOT_CPU_TEST_RUNTIME_H

/* Short-circuit <arch/cpu.h> — the x86_64 machine branch (MSR/asm). */
#define _ARCH_CPU_H

#include <stdint.h>

/* Spy surface — defined in the test TU, referenced by the production
 * object through these externs. */
extern int boot_cpu_test_base_install_calls;
extern void *boot_cpu_test_last_base;

/* Mock arch_set_percpu_base: record, never touch a real MSR. */
static inline void arch_set_percpu_base(void *ptr)
{
    boot_cpu_test_base_install_calls++;
    boot_cpu_test_last_base = ptr;
}

#endif /* BOOT_CPU_TEST_RUNTIME_H */
