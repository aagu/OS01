/* hosttests/mock/ap_work_test_runtime.h
 *
 * Host-only runtime surface for compiling the PRODUCTION
 * kernel/arch/aarch64/smp/ap_work.c on the host (M3 Task 10).
 *
 * Pattern mirrors hosttests/mock/vmm_gate_test_runtime.h: short-circuit
 * the heavy kernel headers with `#define _XXX_H` BEFORE they are included,
 * then declare host-friendly stubs for the symbols ap_work.c needs.
 *
 * IMPORTANT: this header MUST be -included BEFORE any kernel header. The
 * Makefile rule for ap_work_production.o does NOT include test_platform.h,
 * so this header owns the entire stub surface for that TU.
 *
 * Symbols supplied by the test TU (not here):
 *   - kputs()/kputu()                     capture buffer in the test case
 *   - arch_cycle_counter()/arch_cpu_halt()/arch_cpu_pause()  test-controlled
 */
#ifndef OS01_AP_WORK_TEST_RUNTIME_H
#define OS01_AP_WORK_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <config/limits.h>

/* Short-circuit the heavy kernel headers ap_work.c pulls in. */
#define _ARCH_CPU_H                 /* <arch/cpu.h> */
#define _KERNEL_LOG_H
void ap_work_mock_log(const char *fmt, ...);
#define log_err(...) ap_work_mock_log(__VA_ARGS__)
#define OS01_AARCH64_BOOT_LOG_H     /* <arch/aarch64/boot_log.h> */
#define _KERNEL_PERCPU_H            /* <percpu/percpu.h> (P2 ipi_ready) */
#define _ARCH_SPINLOCK_H
#define KERNEL_TASK_H

#include <stdbool.h>

/* P2: the ipi_ready admission gate reads the REAL percpu_t field, so
 * the mock provides a layout-pinned stand-in (field names the
 * production TU touches; the byte-exact layout is pinned by the
 * kernel's own _Static_asserts, not needed here). */
typedef struct ap_work_mock_percpu {
    uint32_t cpu_id;
    uint32_t arch_processor_id;
    uint32_t online;
    uint32_t scheduler_ok;
    uint32_t ipi_ready;
} ap_work_mock_percpu_t;
#define percpu_t ap_work_mock_percpu_t
extern ap_work_mock_percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;

/* P2: the post-READY kick spy (defined in the test case TU). */
void aarch64_sched_kick(unsigned cpu);
extern int mock_kick_calls;
extern unsigned mock_kicked_cpu;

/* UART + cycle/pause/halt stubs — defined in the test case file so the
 * test can capture the WORK-TIMEOUT FATAL line and longjmp out of the
 * for(;;) arch_cpu_halt() terminal loop. */
void kputs(const char *s);
void kputu(uint64_t v);
uint64_t arch_cycle_counter(void);
void arch_cpu_pause(void);
void arch_cpu_halt(void);

#endif /* OS01_AP_WORK_TEST_RUNTIME_H */
