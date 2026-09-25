// ── kernel/arch/aarch64/idle_resume_stub.c ──────────────────────
//
// <sched/task.h> file-scope declares
//   thread_t init_thread = { ..., .rip = (uint64_t)idle_resume, ... };
// unconditionally — no #ifdef __aarch64__ guard. On x86_64,
// kernel/arch/x86_64/entry.S provides `idle_resume` as the entry
// point that returns from idle. On aarch64 phase 2 we have no
// scheduler and no idle thread (Phase 2 #3 = per-CPU timer / SMP
// timer is the prerequisite for an actual scheduler), so this
// stub is never called — its existence is solely to satisfy the
// linker when time/tick.c and time/timer.c are pulled into the
// aarch64 build (which transitively pulls <sched/task.h>'s
// init_thread initializer).
//
// Phase 2 #3 follow-up: replace this stub with a real aarch64
// idle_resume that re-enters the idle loop on each CPU.

#include <stdint.h>

void idle_resume(void)
{
    /* no-op: aarch64 phase 2 has no scheduler; init_thread is
     * dead code (its .rip is never dereferenced because nothing
     * schedules init_thread onto a CPU). */
    for (;;) {
        __asm__ __volatile__("wfi" ::: "memory");
    }
}