/* hosttests/mock/vmm_gate_test_stubs.c
 *
 * Link-time stubs for hosttests that compile production files calling
 * into kernel/arch/aarch64/memory/vmm_gate.c (e.g. test_m1_install
 * compiles boot_direct_map.c, whose M3 change calls
 * aarch64_pt_root_publish before the TTBR1 install).
 *
 * The vmm_gate production object is compiled against
 * mock/vmm_gate_test_runtime.h (16-byte percpu_t tail stub), so
 * percpu_data[] here uses that same type. The stub dtb_cpu_count
 * returns 1: vmm_gate_check() is a no-op pre-SMP (smp_starting == 0),
 * so the value is never consulted by these tests.
 */
#include "vmm_gate_test_runtime.h"

percpu_t percpu_data[NR_CPUS];

uint32_t dtb_cpu_count(void) { return 1; }
