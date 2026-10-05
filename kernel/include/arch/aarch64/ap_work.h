#ifndef OS01_AARCH64_AP_WORK_H
#define OS01_AARCH64_AP_WORK_H

/* kernel/include/arch/aarch64/ap_work.h — M3 AP work-item protocol
 * (spec §6.2). arch-private: the public IPI surface lives in
 * <intr/ipi.h> / <arch/aarch64/ipi.h>; this header is consumed only by
 * the BSP submit side, the AP idle loop (secondary_idle) and the
 * hosttest.
 *
 * Protocol: one statically-allocated slot per CPU (BSS, deliberately
 * NOT inside percpu_t — the BSP writes another CPU's slot directly).
 * Submit writes the payload with plain stores, then publishes READY
 * with a release store; the AP consumes with acquire loads, performs
 * the work, publishes DONE (release); the BSP waits on DONE
 * (acquire), validates the seq (stale-DONE guard) and re-arms the
 * slot to IDLE for the next round. seq starts at 1: the AP's
 * "last consumed seq" is BSS-initialized to 0, so a seq of 0 can
 * never alias a live request.
 *
 * Timeout is terminal (spec §6.2): a wait that blows its deadline
 * prints the WORK-TIMEOUT probe line and halts the CPU — the slot is
 * never re-armed after a timeout.
 */

#include <stdint.h>
#include <stdbool.h>

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

enum ap_work_state { AP_WORK_IDLE = 0, AP_WORK_READY, AP_WORK_DONE };
enum ap_work_cmd { WORK_READ64 = 1, WORK_BARRIER = 2 };

/* M3.6 Task 26 extension commands. These are consumed by the weak
 * ap_work_ext_run() hook below — the production ap_work.c body does
 * not know their payloads; only the kernel selftest
 * (kernel/selftest/test_m3_multicore.c) overrides the hook. Unknown
 * commands without an override are ignored exactly like before
 * (state stays READY, seq does not advance). */
enum ap_work_cmd_ext { WORK_PT_STRESS = 3, WORK_PT_MAP = 4, WORK_PT_ALLOC = 5 };

/* Weak extension dispatcher (M3.6 Task 26): return true when `cmd`
 * was consumed and *out holds the result; false to leave the item
 * pending (legacy behavior for unknown commands). */
bool ap_work_ext_run(uint32_t cmd, uint64_t arg0, uint64_t arg1,
                     uint64_t *out);

struct ap_work {
    /* Plain field, accessed only through the __atomic builtins. This
     * toolchain's __atomic builtins REJECT _Atomic-qualified pointers
     * (clang 23: "address argument to atomic operation must be a
     * pointer to integer"), and <stdatomic.h> is unavailable in the
     * freestanding build (wchar_t) — the same constraint that keeps
     * percpu_t's ipi_ready/tlb_ack_gen tail plain. */
    uint32_t state;
    uint32_t seq;
    uint32_t cmd;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t out;
};

extern struct ap_work ap_work[NR_CPUS];

/* BSP side: publish a work item for `cpu`. seq must be strictly
 * per-CPU increasing and start at 1. */
void ap_work_submit(uint32_t cpu, uint32_t seq, uint32_t cmd,
                    uint64_t arg0, uint64_t arg1);

/* BSP side: wait until the slot reaches DONE (or the deadline in
 * cycles expires → FATAL halt). Returns false on a seq mismatch
 * (stale DONE); on success stores the item's `out` (if requested) and
 * re-arms the slot to IDLE. */
bool ap_work_wait(uint32_t cpu, uint32_t seq, uint64_t *out,
                  uint64_t deadline_cycles);

/* AP side: called from the secondary_idle work loop; consumes at most
 * one READY item whose seq differs from the last consumed one. */
void ap_work_run_one(uint32_t cpu);

#endif /* OS01_AARCH64_AP_WORK_H */
